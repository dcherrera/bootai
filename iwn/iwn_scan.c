/*
 * iwn_scan.c — WiFi scanning for Intel Wireless N (Phase 3)
 *
 * Sends scan command to firmware, polls for beacon/probe response frames,
 * parses SSIDs + RSSI + security info, prints results.
 *
 * Flow: iwn_scan_cmd() → iwn_config_radio() → iwn_send_scan()
 *       → iwn_poll_scan_results() → iwn_print_scan_results()
 *
 * Reference: OpenBSD sys/dev/pci/if_iwn.c (ISC license)
 */

#include "iwn_internal.h"

/* ---- Helpers ---- */

static int iwn_memcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (uint32_t i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

static void print_signed_dec(int val) {
    if (val < 0) {
        efi_print_ascii("-");
        val = -val;
    }
    print_dec((uint32_t)val);
}

/* ---- Drain stale RX notifications ---- */

static void iwn_drain_rx(iwn_dev *dev) {
    struct iwn_rx_status *stat = (struct iwn_rx_status *)dev->rxq.stat.vaddr;
    uint32_t hw_idx = stat->closed_count & 0x0fff;
    int drained = 0;

    efi_print_ascii("  Drain:");
    while ((uint32_t)dev->rxq.cur != hw_idx) {
        void *buf = dev->rxq.buf[dev->rxq.cur].vaddr;
        struct iwn_rx_desc *desc = (struct iwn_rx_desc *)buf;
        efi_print_ascii(" t");
        print_dec((uint32_t)desc->type);
        dev->rxq.cur = (dev->rxq.cur + 1) % IWN_RX_RING_COUNT;
        drained++;
    }

    if (drained > 0) {
        iwn_write(dev, IWN_FH_RX_WPTR,
            ((dev->rxq.cur + IWN_RX_RING_COUNT - 1) %
             IWN_RX_RING_COUNT) & ~7);
    }
    efi_print_ascii(" (");
    print_dec((uint32_t)drained);
    efi_print_ascii(" total)\r\n");

    /* Check if firmware already crashed during runtime init */
    uint32_t intr = iwn_read(dev, IWN_INT);
    efi_print_ascii("  INT before scan: 0x");
    print_hex8((uint8_t)(intr >> 24));
    print_hex8((uint8_t)(intr >> 16));
    print_hex8((uint8_t)(intr >> 8));
    print_hex8((uint8_t)intr);
    if (intr & IWN_INT_SW_ERR)
        efi_print_ascii(" ** SW_ERR already set!");
    if (intr & IWN_INT_HW_ERR)
        efi_print_ascii(" ** HW_ERR already set!");
    efi_print_ascii("\r\n");
    iwn_write(dev, IWN_INT, intr);  /* clear */
    iwn_write(dev, IWN_FH_INT, 0xffffffff);  /* clear FH too */
}

/* ---- Wait for command response on RX ring ---- */
/*
 * Poll RX ring for a response to a sent command (matching type == cmd code).
 * Also handles type-28 TX_DONE notifications. Returns 0 on success, -1 on
 * timeout or firmware error. Drains all notifications up to the response.
 *
 * OpenBSD's iwn_cmd() with async=0 sleeps until the interrupt handler
 * processes the TX_DONE. We approximate this by polling.
 */
int iwn_wait_cmd(iwn_dev *dev, uint8_t cmd_code, const char *cmd_name,
                 int timeout_ms) {
    struct iwn_rx_status *stat = (struct iwn_rx_status *)dev->rxq.stat.vaddr;

    for (int t = 0; t < timeout_ms; t++) {
        uint32_t hw_idx = stat->closed_count & 0x0fff;

        while ((uint32_t)dev->rxq.cur != hw_idx) {
            void *buf = dev->rxq.buf[dev->rxq.cur].vaddr;
            struct iwn_rx_desc *desc = (struct iwn_rx_desc *)buf;

            if (desc->type == cmd_code) {
                /* Got our command response */
                dev->rxq.cur = (dev->rxq.cur + 1) % IWN_RX_RING_COUNT;
                iwn_write(dev, IWN_FH_RX_WPTR,
                    ((dev->rxq.cur + IWN_RX_RING_COUNT - 1) %
                     IWN_RX_RING_COUNT) & ~7);
                return 0;
            }

            /* Log other notification types */
            if (desc->type != 0) {
                efi_print_ascii("  rx[");
                print_dec((uint32_t)desc->type);
                efi_print_ascii("] ");
            }

            dev->rxq.cur = (dev->rxq.cur + 1) % IWN_RX_RING_COUNT;
        }

        /* Update WPTR and clear interrupt status so FH continues DMA */
        iwn_write(dev, IWN_FH_RX_WPTR,
            ((dev->rxq.cur + IWN_RX_RING_COUNT - 1) %
             IWN_RX_RING_COUNT) & ~7);
        iwn_write(dev, IWN_INT, 0xffffffff);
        iwn_write(dev, IWN_FH_INT, 0xffffffff);

        gBS->Stall(1000);  /* 1 ms */
    }

    /* Timeout — check if firmware crashed */
    uint32_t intr = iwn_read(dev, IWN_INT);
    efi_print_ascii("  ");
    efi_print_ascii(cmd_name);
    efi_print_ascii(": timeout (INT=0x");
    print_hex8((uint8_t)(intr >> 24));
    print_hex8((uint8_t)(intr >> 16));
    print_hex8((uint8_t)(intr >> 8));
    print_hex8((uint8_t)intr);
    efi_print_ascii(")\r\n");
    return -1;
}

/* ---- Pre-scan radio configuration ---- */

static int iwn_config_radio(iwn_dev *dev) {
    /* Clear stale interrupt flags */
    iwn_write(dev, IWN_INT, 0xffffffff);

    /* 1. Simple Bluetooth coexistence for 6000 series (must be before RXON)
     * Reference: OpenBSD iwn_send_btcoex() — simple struct for plain 6000.
     * Advanced btcoex (iwn6000_btcoex_config + PRIOTABLE + PROT) is for
     * 6005/6050 only — the 6000 firmware crashes on those. */
    {
        struct iwn_bluetooth bt;
        iwn_memset(&bt, 0, sizeof(bt));
        bt.flags = IWN_BT_COEX_CHAN_ANN | IWN_BT_COEX_BT_PRIO;
        bt.lead_time = IWN_BT_LEAD_TIME_DEF;
        bt.max_kill = IWN_BT_MAX_KILL_DEF;

        if (iwn_cmd(dev, IWN_CMD_BT_COEX, &bt, sizeof(bt)) != 0) {
            efi_print_ascii("  iwn: BT_COEX send failed\r\n");
            return -1;
        }
        if (iwn_wait_cmd(dev, IWN_CMD_BT_COEX, "BT_COEX", 200) != 0)
            return -1;
        efi_print_ascii("  BT_COEX ok\r\n");
    }

    /* 2. TX antenna configuration */
    uint32_t ant = dev->txchainmask;
    if (iwn_cmd(dev, IWN5000_CMD_TX_ANT_CONFIG, &ant, sizeof(ant)) != 0) {
        efi_print_ascii("  iwn: TX_ANT_CONFIG send failed\r\n");
        return -1;
    }
    if (iwn_wait_cmd(dev, IWN5000_CMD_TX_ANT_CONFIG, "TX_ANT", 200) != 0)
        return -1;
    efi_print_ascii("  TX_ANT ok\r\n");

    /* 3. RXON — configure radio for station mode, 2.4 GHz
     * Reference: OpenBSD iwn_config() for unassociated STA state */
    struct iwn_rxon rxon;
    iwn_memset(&rxon, 0, sizeof(rxon));

    /* MAC addresses — myaddr and wlap must both be set */
    iwn_memcpy(rxon.myaddr, dev->mac_addr, 6);
    iwn_memcpy(rxon.wlap, dev->mac_addr, 6);

    /* BSSID left as all-zeros for unassociated state.
     * Firmware rejects multicast BSSID (LSB=1 check). */

    rxon.mode = IWN_MODE_STA;
    rxon.chan = 1;
    rxon.flags = htole32(IWN_RXON_TSF | IWN_RXON_CTS_TO_SELF |
                         IWN_RXON_AUTO | IWN_RXON_24GHZ);
    rxon.filter = htole32(IWN_FILTER_MULTICAST);

    /* RX chain: STA mode — let firmware manage chain selection.
     * Only DRIVER_FORCE + FORCE_SEL is for monitor mode (per OpenBSD). */
    uint8_t chains = dev->rxchainmask;
    rxon.rxchain = htole16(
        IWN_RXCHAIN_VALID(chains) |
        IWN_RXCHAIN_IDLE_COUNT(2) |
        IWN_RXCHAIN_MIMO_COUNT(2));

    rxon.ofdm_mask = 0xff;
    rxon.cck_mask = 0x0f;

    /* HT rate masks (required by 5000+ firmware) */
    rxon.ht_single_mask = 0xff;
    rxon.ht_dual_mask = 0xff;
    rxon.ht_triple_mask = 0xff;

    efi_print_ascii("  RXON: flags=0x");
    {
        uint32_t f = rxon.flags;
        print_hex8((uint8_t)(f >> 24));
        print_hex8((uint8_t)(f >> 16));
        print_hex8((uint8_t)(f >> 8));
        print_hex8((uint8_t)f);
    }
    efi_print_ascii(" chain=0x");
    {
        uint16_t c = rxon.rxchain;
        print_hex8((uint8_t)(c >> 8));
        print_hex8((uint8_t)c);
    }
    efi_print_ascii(" sz=");
    print_dec((uint32_t)IWN5000_RXONSZ);
    efi_print_ascii("\r\n");

    if (iwn_cmd(dev, IWN_CMD_RXON, &rxon, IWN5000_RXONSZ) != 0) {
        efi_print_ascii("  iwn: RXON send failed\r\n");
        return -1;
    }
    if (iwn_wait_cmd(dev, IWN_CMD_RXON, "RXON", 200) != 0)
        return -1;
    efi_print_ascii("  RXON ok\r\n");

    /* 4. Add broadcast node (must be before LINK_QUALITY) */
    struct iwn_node_info node;
    iwn_memset(&node, 0, sizeof(node));
    node.id = IWN5000_ID_BROADCAST;
    iwn_memset(node.macaddr, 0xff, 6);

    if (iwn_cmd(dev, IWN_CMD_ADD_NODE, &node, sizeof(node)) != 0) {
        efi_print_ascii("  iwn: ADD_NODE send failed\r\n");
        return -1;
    }
    if (iwn_wait_cmd(dev, IWN_CMD_ADD_NODE, "ADD_NODE", 200) != 0)
        return -1;
    efi_print_ascii("  ADD_NODE ok\r\n");

    /* 5. Link quality for broadcast node — tells firmware what TX rate
     * to use for probe requests. Without this, scan can't transmit.
     * Reference: OpenBSD iwn_add_broadcast_node() */
    {
        struct iwn_cmd_link_quality linkq;
        iwn_memset(&linkq, 0, sizeof(linkq));
        linkq.id = IWN5000_ID_BROADCAST;
        linkq.antmsk_1stream = (uint8_t)IWN_LSB(dev->txchainmask);
        linkq.antmsk_2stream = IWN_ANT_AB;
        linkq.ampdu_max = IWN_AMPDU_MAX_NO_AGG;
        linkq.ampdu_threshold = 3;
        linkq.ampdu_limit = htole16(4000);

        /* CCK 1 Mbps for all 16 retry entries (2.4 GHz) */
        uint8_t txant = (uint8_t)IWN_LSB(dev->txchainmask);
        for (int i = 0; i < IWN_MAX_TX_RETRIES; i++) {
            linkq.retry[i].plcp = 10;  /* CCK 1 Mbps */
            linkq.retry[i].rflags = IWN_RFLAG_CCK | IWN_RFLAG_ANT(txant);
        }

        if (iwn_cmd(dev, IWN_CMD_LINK_QUALITY, &linkq, sizeof(linkq)) != 0) {
            efi_print_ascii("  iwn: LINK_QUALITY send failed\r\n");
            return -1;
        }
        if (iwn_wait_cmd(dev, IWN_CMD_LINK_QUALITY, "LINKQ", 200) != 0)
            return -1;
        efi_print_ascii("  LINKQ ok\r\n");
    }

    /* 6. TX power — firmware can't transmit without this.
     * Reference: OpenBSD iwn5000_set_txpower() */
    {
        struct iwn5000_cmd_txpower txpow;
        iwn_memset(&txpow, 0, sizeof(txpow));
        txpow.global_limit = 2 * IWN5000_TXPOWER_MAX_DBM;  /* 32 = 16 dBm */
        txpow.flags = IWN5000_TXPOWER_NO_CLOSED;
        txpow.srv_limit = IWN5000_TXPOWER_AUTO;

        if (iwn_cmd(dev, IWN_CMD_TXPOWER_DBM, &txpow, sizeof(txpow)) != 0) {
            efi_print_ascii("  iwn: TXPOWER send failed\r\n");
            return -1;
        }
        if (iwn_wait_cmd(dev, IWN_CMD_TXPOWER_DBM, "TXPOW", 200) != 0)
            return -1;
        efi_print_ascii("  TXPOW ok\r\n");
    }

    /* 7. Critical temperature threshold
     * Reference: OpenBSD iwn_set_critical_temp() */
    {
        iwn_write(dev, IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_CTEMP_STOP_RF);

        struct iwn_critical_temp crit;
        iwn_memset(&crit, 0, sizeof(crit));
        crit.tempR = htole32(110);  /* 110 degrees C */

        if (iwn_cmd(dev, IWN_CMD_SET_CRITICAL_TEMP, &crit, sizeof(crit)) != 0) {
            efi_print_ascii("  iwn: CRIT_TEMP send failed\r\n");
            return -1;
        }
        if (iwn_wait_cmd(dev, IWN_CMD_SET_CRITICAL_TEMP, "CTEMP", 200) != 0)
            return -1;
        efi_print_ascii("  CTEMP ok\r\n");
    }

    /* 8. Set power mode to CAM (last, per OpenBSD ordering) */
    struct iwn_pmgt_cmd pmgt;
    iwn_memset(&pmgt, 0, sizeof(pmgt));  /* all zeros = CAM, no power save */
    if (iwn_cmd(dev, IWN_CMD_SET_POWER_MODE, &pmgt, sizeof(pmgt)) != 0) {
        efi_print_ascii("  iwn: POWER send failed\r\n");
        return -1;
    }
    if (iwn_wait_cmd(dev, IWN_CMD_SET_POWER_MODE, "POWER", 200) != 0)
        return -1;
    efi_print_ascii("  POWER ok\r\n");

    return 0;
}

/* ---- Build and send scan command ---- */

static int iwn_send_scan(iwn_dev *dev) {
    /*
     * Scan command layout (packed into buf[]):
     *   [iwn_scan_hdr]          ~32 bytes
     *   [iwn_cmd_data]          ~44 bytes  (TX params for probe requests)
     *   [20 x iwn_scan_essid]   680 bytes  (all zeroed for broadcast scan)
     *   [probe body]            ~16 bytes  (Supported Rates IE)
     *   [11 x iwn_scan_chan]    132 bytes  (channels 1-11)
     */
    uint8_t buf[2048];
    iwn_memset(buf, 0, sizeof(buf));

    uint8_t *ptr = buf;

    /* ---- Scan header ---- */
    struct iwn_scan_hdr *hdr = (struct iwn_scan_hdr *)ptr;
    hdr->nchan = 11;  /* 2.4 GHz channels 1-11 */
    hdr->quiet_time = htole16(10);       /* move to next chan if no frames in 10ms */
    hdr->quiet_threshold = htole16(1);   /* min 1 packet before quiet_time starts */
    hdr->crc_threshold = IWN_GOOD_CRC_TH_NEVER;

    /* RX chain for scan — force all available antennas (per OpenBSD) */
    uint8_t chains = dev->rxchainmask;
    hdr->rxchain = htole16(
        IWN_RXCHAIN_DRIVER_FORCE |
        IWN_RXCHAIN_VALID(chains) |
        IWN_RXCHAIN_FORCE_SEL(chains) |
        IWN_RXCHAIN_FORCE_MIMO_SEL(chains));

    hdr->max_out = 0;
    hdr->pause_scan = 0;
    hdr->flags = htole32(IWN_RXON_24GHZ | IWN_RXON_AUTO);
    hdr->filter = htole32(IWN_FILTER_MULTICAST | IWN_FILTER_BEACON);

    ptr += sizeof(struct iwn_scan_hdr);

    /* ---- TX params for probe requests ---- */
    struct iwn_cmd_data *tx = (struct iwn_cmd_data *)ptr;
    iwn_memset(tx, 0, sizeof(*tx));
    tx->flags = htole32(IWN_TX_AUTO_SEQ);
    tx->id = IWN5000_ID_BROADCAST;
    tx->lifetime = htole32(IWN_LIFETIME_INFINITE);

    /* CCK 1 Mbps for 2.4 GHz probe requests (per OpenBSD) */
    uint8_t txant = (uint8_t)IWN_LSB(dev->txchainmask);
    tx->plcp = iwn_rates[IWN_RIDX_CCK1].plcp;   /* 10 */
    tx->rflags = IWN_RFLAG_CCK | IWN_RFLAG_ANT(txant);

    ptr += sizeof(struct iwn_cmd_data);

    /* ---- 20 x ESSID entries (all zeroed = broadcast scan) ---- */
    ptr += 20 * sizeof(struct iwn_scan_essid);

    /* ---- Probe request frame body (802.11 header + IEs) ---- */
    /* The firmware expects a complete 802.11 probe request frame here.
     * Reference: OpenBSD iwn_scan() builds ieee80211_frame + IEs. */
    uint8_t *wh = ptr;  /* start of 802.11 frame */

    /* 802.11 frame header (24 bytes) */
    *ptr++ = 0x40; /* FC0: type=0 (mgmt), subtype=4 (probe request) */
    *ptr++ = 0x00; /* FC1: to_ds=0, from_ds=0 */
    *ptr++ = 0x00; *ptr++ = 0x00;  /* Duration (filled by HW) */
    /* DA: broadcast */
    *ptr++ = 0xff; *ptr++ = 0xff; *ptr++ = 0xff;
    *ptr++ = 0xff; *ptr++ = 0xff; *ptr++ = 0xff;
    /* SA: our MAC address */
    iwn_memcpy(ptr, dev->mac_addr, 6); ptr += 6;
    /* BSSID: broadcast */
    *ptr++ = 0xff; *ptr++ = 0xff; *ptr++ = 0xff;
    *ptr++ = 0xff; *ptr++ = 0xff; *ptr++ = 0xff;
    /* Sequence control (filled by HW) */
    *ptr++ = 0x00; *ptr++ = 0x00;

    /* SSID IE: empty for broadcast scan */
    *ptr++ = 0;   /* tag: SSID */
    *ptr++ = 0;   /* length: 0 (broadcast) */

    /* Supported Rates IE (tag 1): 8 rates */
    *ptr++ = 1;   /* tag: Supported Rates */
    *ptr++ = 8;   /* length */
    *ptr++ = 0x82; /* 1 Mbps (basic) */
    *ptr++ = 0x84; /* 2 Mbps (basic) */
    *ptr++ = 0x8b; /* 5.5 Mbps (basic) */
    *ptr++ = 0x96; /* 11 Mbps (basic) */
    *ptr++ = 0x0c; /* 6 Mbps */
    *ptr++ = 0x12; /* 9 Mbps */
    *ptr++ = 0x18; /* 12 Mbps */
    *ptr++ = 0x24; /* 18 Mbps */

    /* Extended Supported Rates IE (tag 50): 4 rates */
    *ptr++ = 50;   /* tag: Extended Supported Rates */
    *ptr++ = 4;    /* length */
    *ptr++ = 0x30; /* 24 Mbps */
    *ptr++ = 0x48; /* 36 Mbps */
    *ptr++ = 0x60; /* 48 Mbps */
    *ptr++ = 0x6c; /* 54 Mbps */

    /* tx->len = entire probe frame (802.11 header + IEs) */
    tx->len = htole16((uint16_t)(ptr - wh));

    /* ---- Channel list: 2.4 GHz channels 1-11 ---- */
    for (int ch = 1; ch <= 11; ch++) {
        struct iwn_scan_chan *chan = (struct iwn_scan_chan *)ptr;
        iwn_memset(chan, 0, sizeof(*chan));
        chan->flags = htole32(IWN_CHAN_ACTIVE);
        chan->chan = htole16((uint16_t)ch);
        chan->rf_gain = 0x28;
        chan->dsp_gain = 0x6e;
        chan->active = htole16(30);   /* 30 ms active dwell */
        chan->passive = htole16(120); /* 120 ms passive dwell (> active) */
        ptr += sizeof(struct iwn_scan_chan);
    }

    /* Set total scan command length */
    uint32_t total_len = (uint32_t)(ptr - buf);
    hdr->len = htole16((uint16_t)total_len);

    /* Send scan command (big command path — ~900 bytes > 136 limit).
     * Scan is async — no command response. Results come as START_SCAN (130),
     * RX_PHY (192), MPDU_RX_DONE (193), STOP_SCAN (132) notifications. */
    efi_print_ascii("  SCAN cmd: len=");
    print_dec(total_len);
    efi_print_ascii("\r\n");

    if (iwn_cmd(dev, IWN_CMD_SCAN, buf, total_len) != 0) {
        efi_print_ascii("  iwn: SCAN command failed\r\n");
        return -1;
    }

    return 0;
}

/* ---- Beacon/probe response frame parser ---- */

static void iwn_parse_beacon(iwn_dev *dev, const uint8_t *frame,
                              uint16_t frame_len, int8_t rssi) {
    /*
     * 802.11 management frame layout:
     *   [0-1]   Frame Control
     *   [2-3]   Duration
     *   [4-9]   DA (destination)
     *   [10-15] SA (source)
     *   [16-21] BSSID
     *   [22-23] Sequence Control
     *   [24-31] Timestamp (8 bytes)
     *   [32-33] Beacon Interval
     *   [34-35] Capability Info
     *   [36+]   Information Elements (TLV)
     */
    if (frame_len < 36)
        return;

    /* Check frame control: must be Beacon (subtype 8) or Probe Response (5) */
    uint16_t fc = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
    uint8_t type = (fc >> 2) & 0x03;    /* should be 0 = management */
    uint8_t subtype = (fc >> 4) & 0x0f;

    if (type != 0)
        return;
    if (subtype != 8 && subtype != 5)  /* beacon or probe response */
        return;

    /* Extract BSSID */
    const uint8_t *bssid = frame + 16;

    /* Check for duplicate — update RSSI if stronger */
    for (int i = 0; i < dev->n_scan_results; i++) {
        if (iwn_memcmp(dev->scan_results[i].bssid, bssid, 6) == 0) {
            if (rssi > dev->scan_results[i].rssi)
                dev->scan_results[i].rssi = rssi;
            return;
        }
    }

    if (dev->n_scan_results >= IWN_MAX_SCAN_RESULTS)
        return;

    /* New BSS — create entry */
    iwn_scan_result *res = &dev->scan_results[dev->n_scan_results];
    iwn_memset(res, 0, sizeof(*res));
    iwn_memcpy(res->bssid, bssid, 6);
    res->rssi = rssi;
    res->security = 0;  /* assume open */

    /* Beacon interval: frame bytes 32-33, little-endian TU */
    res->beacon_interval = (uint16_t)frame[32] | ((uint16_t)frame[33] << 8);

    /* Walk Information Elements (TLV) */
    const uint8_t *ie = frame + 36;
    const uint8_t *ie_end = frame + frame_len;

    while (ie + 2 <= ie_end) {
        uint8_t tag = ie[0];
        uint8_t tlen = ie[1];

        if (ie + 2 + tlen > ie_end)
            break;

        switch (tag) {
        case 0:  /* SSID */
            if (tlen > 0 && tlen <= 32) {
                iwn_memcpy(res->ssid, ie + 2, tlen);
                res->ssid[tlen] = '\0';
                res->ssid_len = tlen;
            }
            break;

        case 3:  /* DS Parameter Set — channel */
            if (tlen >= 1)
                res->channel = ie[2];
            break;

        case 48: /* RSN (WPA2) */
            res->security |= 2;
            break;

        case 221: /* Vendor Specific */
            /* Check for WPA OUI: 00:50:F2:01 */
            if (tlen >= 4 &&
                ie[2] == 0x00 && ie[3] == 0x50 &&
                ie[4] == 0xf2 && ie[5] == 0x01) {
                res->security |= 1;
            }
            break;
        }

        ie += 2 + tlen;
    }

    dev->n_scan_results++;
}

/* ---- Poll RX ring for scan results ---- */

static int iwn_poll_scan_results(iwn_dev *dev, int timeout_ms) {
    struct iwn_rx_status *stat = (struct iwn_rx_status *)dev->rxq.stat.vaddr;
    int8_t cached_rssi = -100;  /* default if no PHY notification before MPDU */
    int scan_started = 0;

    for (int t = 0; t < timeout_ms; t++) {
        uint32_t hw_idx = stat->closed_count & 0x0fff;

        while ((uint32_t)dev->rxq.cur != hw_idx) {
            void *buf = dev->rxq.buf[dev->rxq.cur].vaddr;
            struct iwn_rx_desc *desc = (struct iwn_rx_desc *)buf;
            uint8_t *data = (uint8_t *)buf + sizeof(struct iwn_rx_desc);

            switch (desc->type) {
            case IWN_START_SCAN: {
                struct iwn_start_scan *ss = (struct iwn_start_scan *)data;
                if (!scan_started) {
                    efi_print_ascii("  Scanning ch ");
                    scan_started = 1;
                }
                print_dec((uint32_t)ss->chan);
                efi_print_ascii(" ");
                break;
            }

            case IWN_RX_PHY: {
                /* Cache RSSI from PHY notification — used by next MPDU.
                 * The IWN_RX_PHY data is an iwn_rx_stat, with the
                 * iwn5000_rx_phystat embedded at phybuf offset.
                 * RSSI formula for 5000+: max(chain_rssi) - agc
                 * (IWN_RSSI_TO_DBM is for 4965 only) */
                struct iwn_rx_stat *rxs = (struct iwn_rx_stat *)data;
                struct iwn5000_rx_phystat *phy =
                    (struct iwn5000_rx_phystat *)rxs->phybuf;
                uint8_t agc = (uint8_t)((letoh32(phy->agc) >> 9) & 0x7f);
                int best = -100;
                for (int c = 0; c < 3; c++) {
                    int r = (int)(letoh16(phy->rssi[c]) & 0xff);
                    if (r > best)
                        best = r;
                }
                cached_rssi = (int8_t)(best - (int)agc - IWN_RSSI_TO_DBM);
                break;
            }

            case IWN_MPDU_RX_DONE: {
                /* Parse beacon/probe response frame */
                struct iwn_rx_mpdu *mpdu = (struct iwn_rx_mpdu *)data;
                uint16_t flen = letoh16(mpdu->len);
                uint8_t *frame = data + sizeof(struct iwn_rx_mpdu);

                if (flen > 0 && flen < IWN_RBUF_SIZE - 64)
                    iwn_parse_beacon(dev, frame, flen, cached_rssi);

                /* Reset cached RSSI for next frame */
                cached_rssi = -100;
                break;
            }

            case IWN_STOP_SCAN: {
                struct iwn_stop_scan *ss = (struct iwn_stop_scan *)data;
                efi_print_ascii("\r\n  Scan complete: ");
                print_dec((uint32_t)ss->nchan);
                efi_print_ascii(" channels, ");
                print_dec((uint32_t)dev->n_scan_results);
                efi_print_ascii(" networks found\r\n");

                /* Advance read index before returning */
                dev->rxq.cur = (dev->rxq.cur + 1) % IWN_RX_RING_COUNT;
                iwn_write(dev, IWN_FH_RX_WPTR,
                    ((dev->rxq.cur + IWN_RX_RING_COUNT - 1) %
                     IWN_RX_RING_COUNT) & ~7);
                return 0;
            }

            case 2: {
                /* REPLY_ERROR — firmware rejected a command */
                uint32_t etype = *(uint32_t *)data;
                uint8_t ecmd = data[4];
                uint32_t einfo;
                iwn_memcpy(&einfo, data + 8, 4);
                efi_print_ascii("\r\n  FW_ERR(scan): cmd=");
                print_dec((uint32_t)ecmd);
                efi_print_ascii(" err=0x");
                print_hex8((uint8_t)(etype >> 24));
                print_hex8((uint8_t)(etype >> 16));
                print_hex8((uint8_t)(etype >> 8));
                print_hex8((uint8_t)etype);
                efi_print_ascii(" info=0x");
                print_hex8((uint8_t)(einfo >> 24));
                print_hex8((uint8_t)(einfo >> 16));
                print_hex8((uint8_t)(einfo >> 8));
                print_hex8((uint8_t)einfo);
                efi_print_ascii("\r\n");
                break;
            }

            default:
                /* Log notification type during scan */
                efi_print_ascii("  rx[");
                print_dec((uint32_t)desc->type);
                efi_print_ascii("] ");
                break;
            }

            /* Advance read index */
            dev->rxq.cur = (dev->rxq.cur + 1) % IWN_RX_RING_COUNT;
            iwn_write(dev, IWN_FH_RX_WPTR,
                ((dev->rxq.cur + IWN_RX_RING_COUNT - 1) %
                 IWN_RX_RING_COUNT) & ~7);
        }

        /* Clear interrupt status so FH continues DMA */
        iwn_write(dev, IWN_INT, 0xffffffff);
        iwn_write(dev, IWN_FH_INT, 0xffffffff);

        gBS->Stall(1000);  /* 1 ms */
    }

    /* Check firmware state on timeout */
    {
        uint32_t intr = iwn_read(dev, IWN_INT);
        efi_print_ascii("\r\n  Scan timeout (INT=0x");
        print_hex8((uint8_t)(intr >> 24));
        print_hex8((uint8_t)(intr >> 16));
        print_hex8((uint8_t)(intr >> 8));
        print_hex8((uint8_t)intr);
        efi_print_ascii(")\r\n");
        iwn_write(dev, IWN_INT, intr);
    }
    return -1;
}

/* ---- Print scan results ---- */

static void iwn_print_scan_results(iwn_dev *dev) {
    if (dev->n_scan_results == 0) {
        efi_print_ascii("\r\n  No networks found.\r\n");
        return;
    }

    /* Sort by RSSI (simple insertion sort, descending) */
    for (int i = 1; i < dev->n_scan_results; i++) {
        iwn_scan_result tmp = dev->scan_results[i];
        int j = i - 1;
        while (j >= 0 && dev->scan_results[j].rssi < tmp.rssi) {
            dev->scan_results[j + 1] = dev->scan_results[j];
            j--;
        }
        dev->scan_results[j + 1] = tmp;
    }

    efi_print_ascii("\r\n  #  SSID                              CH  RSSI  Security\r\n");

    for (int i = 0; i < dev->n_scan_results; i++) {
        iwn_scan_result *r = &dev->scan_results[i];

        /* Index */
        efi_print_ascii("  ");
        print_dec((uint32_t)(i + 1));
        if (i + 1 < 10) efi_print_ascii(" ");
        efi_print_ascii(" ");

        /* SSID (left-padded to 32 chars) */
        const char *ssid_str = r->ssid_len > 0 ? r->ssid : "(hidden)";
        int slen = 0;
        const char *sp = ssid_str;
        while (*sp) { slen++; sp++; }

        efi_print_ascii(ssid_str);
        for (int p = slen; p < 34; p++)
            efi_print_ascii(" ");

        /* Channel */
        if (r->channel < 10) efi_print_ascii(" ");
        print_dec((uint32_t)r->channel);
        efi_print_ascii("  ");

        /* RSSI */
        print_signed_dec((int)r->rssi);
        /* Pad RSSI column */
        int rlen = 1;
        int rv = (int)r->rssi;
        if (rv < 0) { rlen++; rv = -rv; }
        if (rv >= 10) rlen++;
        if (rv >= 100) rlen++;
        for (int p = rlen; p < 5; p++)
            efi_print_ascii(" ");
        efi_print_ascii(" ");

        /* Security */
        switch (r->security) {
        case 0: efi_print_ascii("Open"); break;
        case 1: efi_print_ascii("WPA"); break;
        case 2: efi_print_ascii("WPA2"); break;
        case 3: efi_print_ascii("WPA+WPA2"); break;
        default: efi_print_ascii("?"); break;
        }

        efi_print_ascii("\r\n");
    }
}

/* ---- Internal entry point (called with device pointer) ---- */

int iwn_scan(iwn_dev *dev) {
    if (!dev->fw_loaded) {
        efi_print_ascii("  iwn: firmware not loaded\r\n");
        return -1;
    }

    /* Reset scan results */
    dev->n_scan_results = 0;

    /* Drain stale RX notifications from firmware init */
    iwn_drain_rx(dev);

    /* Clear any stale interrupt flags */
    iwn_write(dev, IWN_INT, 0xffffffff);

    /* Configure radio for scanning */
    efi_print_ascii("  Configuring radio...\r\n");
    if (iwn_config_radio(dev) != 0)
        return -1;

    /* Send scan command */
    if (iwn_send_scan(dev) != 0)
        return -1;

    /* Poll for results (8 second timeout) */
    if (iwn_poll_scan_results(dev, 8000) != 0)
        return -1;

    /* Print formatted results */
    iwn_print_scan_results(dev);

    return 0;
}

/* ---- Public entry point (called from REPL) ---- */

void iwn_scan_cmd(void) {
    if (!g_iwn_found) {
        efi_print_ascii("  iwn: not initialized. Run 'iwn' first.\r\n");
        return;
    }

    iwn_scan(&g_iwn);
}
