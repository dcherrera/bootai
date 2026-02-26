/*
 * iwn_assoc.c — 802.11 Authentication + Association for Intel Wireless N
 *
 * Phase 4b: Open System auth → Association Request/Response → post-assoc
 * firmware configuration. After this, the AP starts the EAPOL handshake
 * (handled by Phase 4c).
 *
 * Flow: iwn_connect_cmd() → iwn_connect() → iwn_auth() → iwn_associate()
 *       → iwn_run()
 *
 * Reference: OpenBSD sys/dev/pci/if_iwn.c (ISC license)
 */

#include "iwn_internal.h"

/* ---- Helpers ---- */

static int iwn_strlen(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

static int iwn_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

/* ---- Send an 802.11 management frame on TX ring 0 ---- */
/*
 * Same mechanism as iwn_cmd() (iwn_fw.c:372) but uses ring 0 (data)
 * instead of ring 4 (command). The payload is iwn_cmd_data (TX params)
 * followed by the 802.11 frame. Always uses the big-command path since
 * sizeof(iwn_cmd_data) + frame_len > 136.
 */
static int iwn_tx_frame(iwn_dev *dev, const uint8_t *frame, uint16_t frame_len) {
    iwn_tx_ring *ring = &dev->txq[0];
    int idx = ring->cur;

    /* Build TX command header in ring's cmd buffer */
    struct iwn_tx_cmd *cmd = (struct iwn_tx_cmd *)
        ((uint8_t *)ring->cmd.vaddr + idx * sizeof(struct iwn_tx_cmd));
    iwn_memset(cmd, 0, sizeof(*cmd));
    cmd->code = IWN_CMD_TX_DATA;
    cmd->flags = 0;
    cmd->idx = (uint8_t)idx;
    cmd->qid = 0;

    /* Build iwn_cmd_data TX params immediately after the 4-byte header */
    uint8_t *ring_buf = (uint8_t *)ring->cmd.vaddr +
                        idx * sizeof(struct iwn_tx_cmd);
    struct iwn_cmd_data *tx = (struct iwn_cmd_data *)(ring_buf + 4);
    iwn_memset(tx, 0, sizeof(*tx));

    tx->len = htole16(frame_len);
    tx->flags = htole32(IWN_TX_NEED_ACK | IWN_TX_AUTO_SEQ);
    tx->id = IWN5000_ID_BROADCAST;
    tx->security = 0;
    tx->lifetime = htole32(IWN_LIFETIME_INFINITE);

    /* CCK 1 Mbps for management frames (2.4 GHz) */
    uint8_t txant = (uint8_t)IWN_LSB(dev->txchainmask);
    tx->plcp = 10;  /* CCK 1 Mbps */
    tx->rflags = IWN_RFLAG_CCK | IWN_RFLAG_ANT(txant);

    /* Copy 802.11 frame after iwn_cmd_data */
    iwn_memcpy(ring_buf + 4 + sizeof(struct iwn_cmd_data), frame, frame_len);

    /* Build TX descriptor */
    struct iwn_tx_desc *desc = (struct iwn_tx_desc *)
        ((uint8_t *)ring->desc.vaddr + idx * sizeof(struct iwn_tx_desc));
    iwn_memset(desc, 0, sizeof(*desc));

    uint32_t totlen = 4 + sizeof(struct iwn_cmd_data) + frame_len;
    uint64_t buf_paddr = ring->cmd.paddr + idx * sizeof(struct iwn_tx_cmd);

    desc->nsegs = 1;
    desc->segs[0].addr = IWN_LOADDR(buf_paddr);
    desc->segs[0].len = (uint16_t)(IWN_HIADDR(buf_paddr) |
                                    ((uint16_t)totlen << 4));

    /* Update scheduler byte count table for ring 0 */
    uint16_t *sched = (uint16_t *)dev->sched.vaddr;
    int sched_idx = 0 * IWN5000_SCHED_COUNT + idx;
    uint16_t bc = (uint16_t)(totlen + 3);
    bc = (bc / 4) & 0x0fff;
    sched[sched_idx] = bc;
    sched[sched_idx + IWN_TX_RING_COUNT] = bc;

    /* Kick hardware: advance write pointer for ring 0 */
    ring->cur = (idx + 1) % IWN_TX_RING_COUNT;
    ring->queued++;
    iwn_write(dev, IWN_HBUS_TARG_WRPTR, ring->cur | (0 << 8));

    return 0;
}

/* ---- Poll RX ring for a management frame with specific subtype ---- */
/*
 * Polls RX ring for IWN_MPDU_RX_DONE notifications containing an 802.11
 * management frame matching the expected subtype. Discards beacons, probe
 * responses, and other notifications. Returns 0 on match, -1 on timeout.
 */
static int iwn_poll_mgmt(iwn_dev *dev, uint8_t expected_subtype,
                         uint8_t *frame_out, uint16_t *frame_out_len,
                         int timeout_ms) {
    struct iwn_rx_status *stat = (struct iwn_rx_status *)dev->rxq.stat.vaddr;

    for (int t = 0; t < timeout_ms; t++) {
        uint32_t hw_idx = stat->closed_count & 0x0fff;

        while ((uint32_t)dev->rxq.cur != hw_idx) {
            void *buf = dev->rxq.buf[dev->rxq.cur].vaddr;
            struct iwn_rx_desc *desc = (struct iwn_rx_desc *)buf;
            uint8_t *data = (uint8_t *)buf + sizeof(struct iwn_rx_desc);

            if (desc->type == IWN_MPDU_RX_DONE) {
                struct iwn_rx_mpdu *mpdu = (struct iwn_rx_mpdu *)data;
                uint16_t flen = letoh16(mpdu->len);
                uint8_t *frame = data + sizeof(struct iwn_rx_mpdu);

                if (flen >= 2) {
                    uint8_t fc0 = frame[0];
                    uint8_t type = (fc0 >> 2) & 0x03;
                    uint8_t subtype = (fc0 >> 4) & 0x0f;

                    if (type == 0 && subtype == expected_subtype) {
                        /* Match — copy frame out */
                        if (flen > IWN_RBUF_SIZE - 64)
                            flen = IWN_RBUF_SIZE - 64;
                        if (frame_out)
                            iwn_memcpy(frame_out, frame, flen);
                        if (frame_out_len)
                            *frame_out_len = flen;

                        dev->rxq.cur = (dev->rxq.cur + 1) % IWN_RX_RING_COUNT;
                        iwn_write(dev, IWN_FH_RX_WPTR,
                            ((dev->rxq.cur + IWN_RX_RING_COUNT - 1) %
                             IWN_RX_RING_COUNT) & ~7);
                        return 0;
                    }
                }
            } else if (desc->type == IWN_TX_DONE) {
                /* TX completion — expected, silently consume */
            } else if (desc->type != 0) {
                /* Silently consume beacons and other noise */
            }

            dev->rxq.cur = (dev->rxq.cur + 1) % IWN_RX_RING_COUNT;
        }

        /* Update WPTR and clear interrupt status */
        iwn_write(dev, IWN_FH_RX_WPTR,
            ((dev->rxq.cur + IWN_RX_RING_COUNT - 1) %
             IWN_RX_RING_COUNT) & ~7);
        iwn_write(dev, IWN_INT, 0xffffffff);
        iwn_write(dev, IWN_FH_INT, 0xffffffff);

        gBS->Stall(1000);  /* 1 ms */
    }

    return -1;  /* timeout */
}

/* ---- Authentication: configure radio + send Open System auth ---- */

static int iwn_auth(iwn_dev *dev) {
    efi_print_ascii("  Auth: configuring radio...\r\n");

    /* 1. RXON — station mode, target BSSID + channel */
    struct iwn_rxon rxon;
    iwn_memset(&rxon, 0, sizeof(rxon));
    iwn_memcpy(rxon.myaddr, dev->mac_addr, 6);
    iwn_memcpy(rxon.wlap, dev->mac_addr, 6);
    iwn_memcpy(rxon.bssid, dev->target_bssid, 6);
    rxon.mode = IWN_MODE_STA;
    rxon.chan = dev->target_channel;
    rxon.flags = htole32(IWN_RXON_TSF | IWN_RXON_CTS_TO_SELF |
                         IWN_RXON_AUTO | IWN_RXON_24GHZ);
    rxon.filter = htole32(IWN_FILTER_MULTICAST);

    uint8_t chains = dev->rxchainmask;
    rxon.rxchain = htole16(
        IWN_RXCHAIN_VALID(chains) |
        IWN_RXCHAIN_IDLE_COUNT(2) |
        IWN_RXCHAIN_MIMO_COUNT(2));

    rxon.ofdm_mask = 0xff;
    rxon.cck_mask = 0x0f;
    rxon.ht_single_mask = 0xff;
    rxon.ht_dual_mask = 0xff;
    rxon.ht_triple_mask = 0xff;

    /* Not associated yet: associd=0, no IWN_FILTER_BSS */

    if (iwn_cmd(dev, IWN_CMD_RXON, &rxon, IWN5000_RXONSZ) != 0) {
        efi_print_ascii("  Auth: RXON send failed\r\n");
        return -1;
    }
    if (iwn_wait_cmd(dev, IWN_CMD_RXON, "RXON", 200) != 0)
        return -1;
    efi_print_ascii("  RXON ok (ch ");
    print_dec((uint32_t)dev->target_channel);
    efi_print_ascii(")\r\n");

    /* 2. TIMING — beacon interval from scan */
    struct iwn_cmd_timing timing;
    iwn_memset(&timing, 0, sizeof(timing));
    uint16_t bintval = dev->target_beacon_interval;
    if (bintval == 0) bintval = 100;  /* default 100 TU */
    timing.bintval = htole16(bintval);
    timing.lintval = htole16(bintval);
    timing.binitval = htole32((uint32_t)bintval * 1024);

    if (iwn_cmd(dev, IWN_CMD_TIMING, &timing, sizeof(timing)) != 0) {
        efi_print_ascii("  Auth: TIMING send failed\r\n");
        return -1;
    }
    if (iwn_wait_cmd(dev, IWN_CMD_TIMING, "TIMING", 200) != 0)
        return -1;
    efi_print_ascii("  TIMING ok\r\n");

    /* 3. ADD_NODE (broadcast, id=15) */
    struct iwn_node_info node;
    iwn_memset(&node, 0, sizeof(node));
    node.id = IWN5000_ID_BROADCAST;
    iwn_memset(node.macaddr, 0xff, 6);

    if (iwn_cmd(dev, IWN_CMD_ADD_NODE, &node, sizeof(node)) != 0) {
        efi_print_ascii("  Auth: ADD_NODE send failed\r\n");
        return -1;
    }
    if (iwn_wait_cmd(dev, IWN_CMD_ADD_NODE, "ADD_NODE", 200) != 0)
        return -1;
    efi_print_ascii("  ADD_NODE ok\r\n");

    /* 4. LINK_QUALITY (broadcast, CCK 1 Mbps) */
    {
        struct iwn_cmd_link_quality linkq;
        iwn_memset(&linkq, 0, sizeof(linkq));
        linkq.id = IWN5000_ID_BROADCAST;
        linkq.antmsk_1stream = (uint8_t)IWN_LSB(dev->txchainmask);
        linkq.antmsk_2stream = IWN_ANT_AB;
        linkq.ampdu_max = IWN_AMPDU_MAX_NO_AGG;
        linkq.ampdu_threshold = 3;
        linkq.ampdu_limit = htole16(4000);

        uint8_t txant = (uint8_t)IWN_LSB(dev->txchainmask);
        for (int i = 0; i < IWN_MAX_TX_RETRIES; i++) {
            linkq.retry[i].plcp = 10;  /* CCK 1 Mbps */
            linkq.retry[i].rflags = IWN_RFLAG_CCK | IWN_RFLAG_ANT(txant);
        }

        if (iwn_cmd(dev, IWN_CMD_LINK_QUALITY, &linkq, sizeof(linkq)) != 0) {
            efi_print_ascii("  Auth: LINKQ send failed\r\n");
            return -1;
        }
        if (iwn_wait_cmd(dev, IWN_CMD_LINK_QUALITY, "LINKQ", 200) != 0)
            return -1;
        efi_print_ascii("  LINKQ ok\r\n");
    }

    /* 5. TX Auth Request (Open System, seq=1) */
    efi_print_ascii("  Sending auth request...\r\n");
    {
        uint8_t auth_frame[30];
        iwn_memset(auth_frame, 0, sizeof(auth_frame));

        /* Frame Control: management, subtype=11 (auth) */
        auth_frame[0] = 0xB0;  /* FC byte 0: type=0(mgmt), subtype=11 */
        auth_frame[1] = 0x00;  /* FC byte 1 */

        /* Duration: 0 */
        /* Addr1 (DA) = target BSSID */
        iwn_memcpy(auth_frame + 4, dev->target_bssid, 6);
        /* Addr2 (SA) = our MAC */
        iwn_memcpy(auth_frame + 10, dev->mac_addr, 6);
        /* Addr3 (BSSID) = target BSSID */
        iwn_memcpy(auth_frame + 16, dev->target_bssid, 6);
        /* Seq ctrl = 0 (firmware fills via AUTO_SEQ) */

        /* Auth body (starts at byte 24) */
        /* Auth Algorithm: 0x0000 = Open System */
        auth_frame[24] = 0x00;
        auth_frame[25] = 0x00;
        /* Auth Seq Number: 0x0001 */
        auth_frame[26] = 0x01;
        auth_frame[27] = 0x00;
        /* Status: 0x0000 */
        auth_frame[28] = 0x00;
        auth_frame[29] = 0x00;

        if (iwn_tx_frame(dev, auth_frame, 30) != 0) {
            efi_print_ascii("  Auth: TX failed\r\n");
            return -1;
        }
    }

    /* 6. Poll for Auth Response (subtype=11, management) */
    {
        uint8_t resp[256];
        uint16_t resp_len = 0;

        if (iwn_poll_mgmt(dev, 11, resp, &resp_len, 3000) != 0) {
            efi_print_ascii("  Auth: no response (timeout)\r\n");
            return -1;
        }

        /* Verify: auth seq == 2, status == 0 */
        if (resp_len < 30) {
            efi_print_ascii("  Auth: response too short\r\n");
            return -1;
        }

        uint16_t auth_seq = (uint16_t)resp[26] | ((uint16_t)resp[27] << 8);
        uint16_t auth_status = (uint16_t)resp[28] | ((uint16_t)resp[29] << 8);

        if (auth_seq != 2) {
            efi_print_ascii("  Auth: unexpected seq ");
            print_dec((uint32_t)auth_seq);
            efi_print_ascii("\r\n");
            return -1;
        }

        if (auth_status != 0) {
            efi_print_ascii("  Auth: rejected, status=");
            print_dec((uint32_t)auth_status);
            efi_print_ascii("\r\n");
            return -1;
        }

        efi_print_ascii("  Auth ok\r\n");
    }

    dev->state = IWN_STATE_AUTH;
    return 0;
}

/* ---- Association: send Assoc Request, parse Assoc Response ---- */

static int iwn_associate(iwn_dev *dev) {
    efi_print_ascii("  Sending assoc request...\r\n");

    /* Build Association Request frame */
    uint8_t frame[256];
    uint8_t *ptr = frame;
    iwn_memset(frame, 0, sizeof(frame));

    /* Frame Control: management, subtype=0 (assoc request) */
    *ptr++ = 0x00;  /* FC byte 0: type=0(mgmt), subtype=0 */
    *ptr++ = 0x00;  /* FC byte 1 */

    /* Duration: 0 */
    *ptr++ = 0x00; *ptr++ = 0x00;

    /* Addr1 (DA) = BSSID */
    iwn_memcpy(ptr, dev->target_bssid, 6); ptr += 6;
    /* Addr2 (SA) = our MAC */
    iwn_memcpy(ptr, dev->mac_addr, 6); ptr += 6;
    /* Addr3 (BSSID) = BSSID */
    iwn_memcpy(ptr, dev->target_bssid, 6); ptr += 6;

    /* Sequence Control: 0 (firmware fills via AUTO_SEQ) */
    *ptr++ = 0x00; *ptr++ = 0x00;

    /* ---- Fixed fields (Assoc Request body, starts at byte 24) ---- */

    /* Capability Info: ESS | Short Preamble | Short Slot Time */
    uint16_t capability = 0x0431;
    *ptr++ = (uint8_t)(capability & 0xff);
    *ptr++ = (uint8_t)(capability >> 8);

    /* Listen Interval */
    *ptr++ = 10; *ptr++ = 0;

    /* ---- Information Elements ---- */

    /* SSID IE (tag 0) */
    *ptr++ = 0;  /* tag */
    *ptr++ = dev->target_ssid_len;
    iwn_memcpy(ptr, dev->target_ssid, dev->target_ssid_len);
    ptr += dev->target_ssid_len;

    /* Supported Rates IE (tag 1): 8 rates */
    *ptr++ = 1;    /* tag */
    *ptr++ = 8;    /* length */
    *ptr++ = 0x82; /* 1 Mbps (basic) */
    *ptr++ = 0x84; /* 2 Mbps (basic) */
    *ptr++ = 0x8b; /* 5.5 Mbps (basic) */
    *ptr++ = 0x96; /* 11 Mbps (basic) */
    *ptr++ = 0x0c; /* 6 Mbps */
    *ptr++ = 0x12; /* 9 Mbps */
    *ptr++ = 0x18; /* 12 Mbps */
    *ptr++ = 0x24; /* 18 Mbps */

    /* Extended Supported Rates IE (tag 50): 4 rates */
    *ptr++ = 50;   /* tag */
    *ptr++ = 4;    /* length */
    *ptr++ = 0x30; /* 24 Mbps */
    *ptr++ = 0x48; /* 36 Mbps */
    *ptr++ = 0x60; /* 48 Mbps */
    *ptr++ = 0x6c; /* 54 Mbps */

    /* RSN IE (tag 48) — only for WPA2 */
    if (dev->target_security & 2) {
        uint8_t *rsn_start = ptr;

        *ptr++ = 48;   /* tag: RSN */
        *ptr++ = 20;   /* length (will be filled below) */

        /* RSN Version: 1 */
        *ptr++ = 0x01; *ptr++ = 0x00;

        /* Group Cipher Suite: 00:0F:AC:04 (CCMP) */
        *ptr++ = 0x00; *ptr++ = 0x0F; *ptr++ = 0xAC; *ptr++ = 0x04;

        /* Pairwise Cipher Suite Count: 1 */
        *ptr++ = 0x01; *ptr++ = 0x00;
        /* Pairwise Cipher Suite: 00:0F:AC:04 (CCMP) */
        *ptr++ = 0x00; *ptr++ = 0x0F; *ptr++ = 0xAC; *ptr++ = 0x04;

        /* AKM Suite Count: 1 */
        *ptr++ = 0x01; *ptr++ = 0x00;
        /* AKM Suite: 00:0F:AC:02 (PSK) */
        *ptr++ = 0x00; *ptr++ = 0x0F; *ptr++ = 0xAC; *ptr++ = 0x02;

        /* RSN Capabilities: 0x0000 */
        *ptr++ = 0x00; *ptr++ = 0x00;

        /* Save RSN IE for Phase 4c EAPOL replay */
        dev->rsn_ie_len = (uint8_t)(ptr - rsn_start);
        iwn_memcpy(dev->rsn_ie, rsn_start, dev->rsn_ie_len);
    }

    uint16_t frame_len = (uint16_t)(ptr - frame);

    if (iwn_tx_frame(dev, frame, frame_len) != 0) {
        efi_print_ascii("  Assoc: TX failed\r\n");
        return -1;
    }

    /* Poll for Association Response (subtype=1) */
    {
        uint8_t resp[512];
        uint16_t resp_len = 0;

        if (iwn_poll_mgmt(dev, 1, resp, &resp_len, 3000) != 0) {
            efi_print_ascii("  Assoc: no response (timeout)\r\n");
            return -1;
        }

        /* Assoc Response fixed fields start at byte 24:
         *   [24-25] Capability Info
         *   [26-27] Status Code
         *   [28-29] Association ID (AID)
         */
        if (resp_len < 30) {
            efi_print_ascii("  Assoc: response too short\r\n");
            return -1;
        }

        uint16_t assoc_status = (uint16_t)resp[26] | ((uint16_t)resp[27] << 8);
        uint16_t aid = ((uint16_t)resp[28] | ((uint16_t)resp[29] << 8)) & 0x3FFF;

        if (assoc_status != 0) {
            efi_print_ascii("  Assoc: rejected, status=");
            print_dec((uint32_t)assoc_status);
            efi_print_ascii("\r\n");
            return -1;
        }

        dev->assoc_id = aid;
        efi_print_ascii("  Assoc ok (AID=");
        print_dec((uint32_t)aid);
        efi_print_ascii(")\r\n");
    }

    dev->state = IWN_STATE_ASSOC;
    return 0;
}

/* ---- Post-association firmware configuration ---- */

static int iwn_run(iwn_dev *dev) {
    efi_print_ascii("  Post-assoc config...\r\n");

    /* 1. Resend full RXON with associd + IWN_FILTER_BSS */
    struct iwn_rxon rxon;
    iwn_memset(&rxon, 0, sizeof(rxon));
    iwn_memcpy(rxon.myaddr, dev->mac_addr, 6);
    iwn_memcpy(rxon.wlap, dev->mac_addr, 6);
    iwn_memcpy(rxon.bssid, dev->target_bssid, 6);
    rxon.mode = IWN_MODE_STA;
    rxon.chan = dev->target_channel;
    rxon.flags = htole32(IWN_RXON_TSF | IWN_RXON_CTS_TO_SELF |
                         IWN_RXON_AUTO | IWN_RXON_24GHZ |
                         IWN_RXON_SHPREAMBLE | IWN_RXON_SHSLOT);
    rxon.filter = htole32(IWN_FILTER_MULTICAST | IWN_FILTER_BSS);
    rxon.associd = htole16(dev->assoc_id | 0xC000);

    uint8_t chains = dev->rxchainmask;
    rxon.rxchain = htole16(
        IWN_RXCHAIN_VALID(chains) |
        IWN_RXCHAIN_IDLE_COUNT(2) |
        IWN_RXCHAIN_MIMO_COUNT(2));

    rxon.ofdm_mask = 0xff;
    rxon.cck_mask = 0x0f;
    rxon.ht_single_mask = 0xff;
    rxon.ht_dual_mask = 0xff;
    rxon.ht_triple_mask = 0xff;

    if (iwn_cmd(dev, IWN_CMD_RXON, &rxon, IWN5000_RXONSZ) != 0) {
        efi_print_ascii("  Run: RXON send failed\r\n");
        return -1;
    }
    if (iwn_wait_cmd(dev, IWN_CMD_RXON, "RXON(assoc)", 200) != 0)
        return -1;
    efi_print_ascii("  RXON(assoc) ok\r\n");

    /* 2. ADD_NODE for AP (id=0, BSS node) */
    struct iwn_node_info node;
    iwn_memset(&node, 0, sizeof(node));
    node.id = IWN_ID_BSS;
    iwn_memcpy(node.macaddr, dev->target_bssid, 6);

    if (iwn_cmd(dev, IWN_CMD_ADD_NODE, &node, sizeof(node)) != 0) {
        efi_print_ascii("  Run: ADD_NODE(AP) send failed\r\n");
        return -1;
    }
    if (iwn_wait_cmd(dev, IWN_CMD_ADD_NODE, "ADD_NODE(AP)", 200) != 0)
        return -1;
    efi_print_ascii("  ADD_NODE(AP) ok\r\n");

    /* 3. LINK_QUALITY for AP — full descending rate table */
    {
        struct iwn_cmd_link_quality linkq;
        iwn_memset(&linkq, 0, sizeof(linkq));
        linkq.id = IWN_ID_BSS;
        linkq.antmsk_1stream = (uint8_t)IWN_LSB(dev->txchainmask);
        linkq.antmsk_2stream = IWN_ANT_AB;
        linkq.ampdu_max = IWN_AMPDU_MAX_NO_AGG;
        linkq.ampdu_threshold = 3;
        linkq.ampdu_limit = htole16(4000);

        uint8_t txant = (uint8_t)IWN_LSB(dev->txchainmask);

        /* Descending rate table: OFDM 54→6, then CCK 11→1 */
        /* OFDM rates (from iwn_rates[]): indices 11,10,9,8,7,6,5,4 */
        static const uint8_t ofdm_plcp[] = {
            0x03, /* 54 Mbps */
            0x01, /* 48 Mbps */
            0x0b, /* 36 Mbps */
            0x09, /* 24 Mbps */
            0x07, /* 18 Mbps */
            0x05, /* 12 Mbps */
            0x0f, /* 9 Mbps */
            0x0d, /* 6 Mbps */
        };
        static const uint8_t cck_plcp[] = {
            110,  /* 11 Mbps */
            55,   /* 5.5 Mbps */
            20,   /* 2 Mbps */
            10,   /* 1 Mbps */
        };

        int r = 0;
        /* OFDM entries */
        for (int i = 0; i < 8 && r < IWN_MAX_TX_RETRIES; i++, r++) {
            linkq.retry[r].plcp = ofdm_plcp[i];
            linkq.retry[r].rflags = IWN_RFLAG_ANT(txant);
        }
        /* CCK entries */
        for (int i = 0; i < 4 && r < IWN_MAX_TX_RETRIES; i++, r++) {
            linkq.retry[r].plcp = cck_plcp[i];
            linkq.retry[r].rflags = IWN_RFLAG_CCK | IWN_RFLAG_ANT(txant);
        }
        /* Fill remaining with CCK 1 Mbps */
        for (; r < IWN_MAX_TX_RETRIES; r++) {
            linkq.retry[r].plcp = 10;
            linkq.retry[r].rflags = IWN_RFLAG_CCK | IWN_RFLAG_ANT(txant);
        }

        if (iwn_cmd(dev, IWN_CMD_LINK_QUALITY, &linkq, sizeof(linkq)) != 0) {
            efi_print_ascii("  Run: LINKQ(AP) send failed\r\n");
            return -1;
        }
        if (iwn_wait_cmd(dev, IWN_CMD_LINK_QUALITY, "LINKQ(AP)", 200) != 0)
            return -1;
        efi_print_ascii("  LINKQ(AP) ok\r\n");
    }

    dev->state = IWN_STATE_RUN;
    return 0;
}

/* ---- Top-level connect (internal, takes device pointer) ---- */

int iwn_connect(iwn_dev *dev, const char *ssid) {
    if (!dev->fw_loaded) {
        efi_print_ascii("  iwn: firmware not loaded\r\n");
        return -1;
    }

    if (dev->n_scan_results == 0) {
        efi_print_ascii("  iwn: no scan results — run 'iwn scan' first\r\n");
        return -1;
    }

    /* Find SSID in scan results */
    int found = -1;
    int ssid_len = iwn_strlen(ssid);
    for (int i = 0; i < dev->n_scan_results; i++) {
        if (dev->scan_results[i].ssid_len == (uint8_t)ssid_len &&
            iwn_strcmp(dev->scan_results[i].ssid, ssid) == 0) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        efi_print_ascii("  iwn: SSID '");
        efi_print_ascii(ssid);
        efi_print_ascii("' not found in scan results\r\n");
        return -1;
    }

    /* Copy target AP info */
    iwn_scan_result *ap = &dev->scan_results[found];
    iwn_memcpy(dev->target_bssid, ap->bssid, 6);
    iwn_memcpy(dev->target_ssid, ap->ssid, ap->ssid_len + 1);
    dev->target_ssid_len = ap->ssid_len;
    dev->target_channel = ap->channel;
    dev->target_beacon_interval = ap->beacon_interval;
    dev->target_security = ap->security;

    efi_print_ascii("  Connecting to '");
    efi_print_ascii(dev->target_ssid);
    efi_print_ascii("' ch ");
    print_dec((uint32_t)dev->target_channel);
    efi_print_ascii(" BSSID ");
    for (int i = 0; i < 6; i++) {
        print_hex8(dev->target_bssid[i]);
        if (i < 5) efi_print_ascii(":");
    }
    efi_print_ascii("\r\n");

    /* WPA2 requires a password */
    if ((dev->target_security & 2) && dev->password[0] == '\0') {
        efi_print_ascii("  iwn: WPA2 network — set password with 'iwn pass <password>' first\r\n");
        return -1;
    }

    /* Run the connection state machine */
    if (iwn_auth(dev) != 0)
        return -1;
    if (iwn_associate(dev) != 0)
        return -1;
    if (iwn_run(dev) != 0)
        return -1;

    efi_print_ascii("  Associated with '");
    efi_print_ascii(dev->target_ssid);
    efi_print_ascii("' (AID=");
    print_dec((uint32_t)dev->assoc_id);
    efi_print_ascii(")\r\n");

    /* WPA2: run EAPOL 4-way handshake */
    if (dev->target_security & 2) {
        if (dev->password[0] == '\0') {
            efi_print_ascii("  WPA2: no password — AP will deauth\r\n");
            return -1;
        }
        if (iwn_eapol_handshake(dev) != 0)
            return -1;
    }

    /* Phase 5: Install SNP + start DHCP */
    iwn_snp_install();
    efi_net_init();

    return 0;
}

/* ---- Public entry point (called from REPL) ---- */

void iwn_connect_cmd(const char *ssid) {
    if (!g_iwn_found) {
        efi_print_ascii("  iwn: not initialized. Run 'iwn' first.\r\n");
        return;
    }

    iwn_connect(&g_iwn, ssid);
}

void iwn_set_password(const char *password) {
    if (!g_iwn_found) {
        efi_print_ascii("  iwn: not initialized. Run 'iwn' first.\r\n");
        return;
    }

    int i = 0;
    while (password[i] && i < 63) {
        g_iwn.password[i] = password[i];
        i++;
    }
    g_iwn.password[i] = '\0';
    efi_print_ascii("  Password set.\r\n");
}
