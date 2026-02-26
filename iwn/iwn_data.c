/*
 * iwn_data.c -- 802.11 Data Frame TX/RX for Intel Wireless N
 *
 * Phase 4d: Ethernet-to-802.11 encapsulation (TX) and 802.11-to-Ethernet
 * decapsulation (RX), plus a software receive buffer for Phase 5 (SNP).
 *
 * The firmware handles CCMP encrypt/decrypt after keys are installed
 * (Phase 4c). We just set the security field in the TX descriptor.
 *
 * Reference: OpenBSD sys/dev/pci/if_iwn.c (ISC license)
 */

#include "iwn_internal.h"

/* LLC/SNAP header prefix (OUI 00:00:00) */
static const uint8_t llc_snap_prefix[6] = {
    0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00
};

/* =====================================================================
 * iwn_tx_raw -- Low-level TX on ring 0
 *
 * Submits a pre-built 802.11 frame on TX ring 0. This is the canonical
 * version that iwn_tx_frame() and iwn_tx_eapol() each duplicated.
 * ===================================================================== */

int iwn_tx_raw(iwn_dev *dev, const uint8_t *frame, uint16_t frame_len,
               uint8_t node_id, uint8_t security) {
    iwn_tx_ring *ring = &dev->txq[0];
    int idx = ring->cur;

    /* Build TX command header */
    struct iwn_tx_cmd *cmd = (struct iwn_tx_cmd *)
        ((uint8_t *)ring->cmd.vaddr + idx * sizeof(struct iwn_tx_cmd));
    iwn_memset(cmd, 0, sizeof(*cmd));
    cmd->code = IWN_CMD_TX_DATA;
    cmd->flags = 0;
    cmd->idx = (uint8_t)idx;
    cmd->qid = 0;

    /* Build iwn_cmd_data TX params after the 4-byte header */
    uint8_t *ring_buf = (uint8_t *)ring->cmd.vaddr +
                        idx * sizeof(struct iwn_tx_cmd);
    struct iwn_cmd_data *tx = (struct iwn_cmd_data *)(ring_buf + 4);
    iwn_memset(tx, 0, sizeof(*tx));

    tx->len = htole16(frame_len);
    tx->flags = htole32(IWN_TX_NEED_ACK | IWN_TX_AUTO_SEQ);
    tx->id = node_id;
    tx->security = security;
    tx->lifetime = htole32(IWN_LIFETIME_INFINITE);

    /* OFDM 24 Mbps for data frames -- firmware uses LINK_QUALITY
     * retry chain for rate fallback (54->48->36->24->...->CCK 1) */
    uint8_t txant = (uint8_t)IWN_LSB(dev->txchainmask);
    tx->plcp = 0x09;  /* OFDM 24 Mbps */
    tx->rflags = IWN_RFLAG_ANT(txant);

    /* Copy frame after iwn_cmd_data */
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

    /* Kick hardware */
    ring->cur = (idx + 1) % IWN_TX_RING_COUNT;
    ring->queued++;
    iwn_write(dev, IWN_HBUS_TARG_WRPTR, ring->cur | (0 << 8));

    return 0;
}

/* =====================================================================
 * iwn_encap_tx -- High-level data TX
 *
 * Takes Ethernet-style parameters, builds 802.11 data frame + LLC/SNAP,
 * calls iwn_tx_raw().
 * ===================================================================== */

int iwn_encap_tx(iwn_dev *dev, const uint8_t *dst_mac,
                 uint16_t ethertype, const uint8_t *payload,
                 uint16_t payload_len) {
    /* Max frame: 24 (802.11 hdr) + 8 (LLC/SNAP) + payload */
    uint16_t frame_len = 24 + 8 + payload_len;
    uint8_t frame[1600];

    if (frame_len > sizeof(frame))
        return -1;

    iwn_memset(frame, 0, 32); /* zero header area */

    /* 802.11 Data header (24 bytes) */
    frame[0] = 0x08;  /* FC byte 0: type=2 (data), subtype=0 */

    if (dev->keys_installed)
        frame[1] = 0x41;  /* FC byte 1: ToDS=1, Protected=1 */
    else
        frame[1] = 0x01;  /* FC byte 1: ToDS=1 */

    /* Duration: 0 (bytes 2-3) */

    /* Addr1 (RA) = AP BSSID -- AP receives the frame */
    iwn_memcpy(frame + 4, dev->target_bssid, 6);
    /* Addr2 (TA) = our MAC -- we transmit */
    iwn_memcpy(frame + 10, dev->mac_addr, 6);
    /* Addr3 (DA) = final destination */
    iwn_memcpy(frame + 16, dst_mac, 6);
    /* SeqCtrl = 0 (bytes 22-23) -- firmware fills via AUTO_SEQ */

    /* LLC/SNAP header (8 bytes) */
    iwn_memcpy(frame + 24, llc_snap_prefix, 6);
    frame[30] = (uint8_t)(ethertype >> 8);   /* big-endian ethertype */
    frame[31] = (uint8_t)(ethertype);

    /* Payload */
    iwn_memcpy(frame + 32, payload, payload_len);

    /* Determine node ID and security:
     * - Broadcast/multicast (dst[0] & 1): node_id=15 (GTK), security=CCMP
     * - Unicast: node_id=0 (PTK), security=CCMP
     * - If keys not installed: security=0 */
    uint8_t node_id;
    uint8_t security;

    if (dst_mac[0] & 1) {
        node_id = IWN5000_ID_BROADCAST;
    } else {
        node_id = IWN_ID_BSS;
    }

    if (dev->keys_installed)
        security = IWN_CIPHER_CCMP;
    else
        security = 0;

    return iwn_tx_raw(dev, frame, frame_len, node_id, security);
}

/* =====================================================================
 * iwn_rx_poll -- Drain HW RX ring into software frame buffer
 *
 * Non-blocking: reads all available frames from the hardware RX ring
 * and queues data frames into dev->rx_buf[]. Returns the number of
 * frames queued.
 * ===================================================================== */

int iwn_rx_poll(iwn_dev *dev) {
    struct iwn_rx_status *stat = (struct iwn_rx_status *)dev->rxq.stat.vaddr;
    uint32_t hw_idx = stat->closed_count & 0x0fff;
    int queued = 0;

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

                /* Deauth: type=0 (mgmt), subtype=12 */
                if (type == 0 && subtype == 12) {
                    efi_print_ascii("  DATA: AP sent deauth!\r\n");
                    dev->state = IWN_STATE_INIT;
                    dev->keys_installed = 0;
                    goto advance;
                }

                /* Data frame: type=2, subtype=0 (data) or 8 (QoS data) */
                if (type == 2 && (subtype == 0 || subtype == 8)) {
                    /* Determine header length (QoS has 2 extra bytes) */
                    int hdr_len = 24;
                    if (subtype == 8)
                        hdr_len = 26;

                    /* Need at least header + LLC/SNAP */
                    if (flen < (uint16_t)(hdr_len + 8))
                        goto advance;

                    /* Skip EAPOL frames (ethertype 0x888E) */
                    uint8_t *snap = frame + hdr_len;
                    uint16_t ether = ((uint16_t)snap[6] << 8) | snap[7];
                    if (ether == 0x888E)
                        goto advance;

                    /* Verify LLC/SNAP prefix */
                    int snap_ok = 1;
                    for (int i = 0; i < 6; i++) {
                        if (snap[i] != llc_snap_prefix[i]) {
                            snap_ok = 0;
                            break;
                        }
                    }
                    if (!snap_ok)
                        goto advance;

                    /* Check if RX buffer has space */
                    int next_tail = (dev->rx_tail + 1) % IWN_RX_BUF_COUNT;
                    if (next_tail == dev->rx_head)
                        goto advance;  /* buffer full, drop */

                    /* Decapsulate: build Ethernet frame from 802.11
                     *
                     * FromDS (AP -> us):
                     *   Addr1 (RA) = our MAC (destination)
                     *   Addr2 (TA) = AP BSSID (transmitter)
                     *   Addr3 (SA) = original source MAC
                     */
                    uint8_t *eth = dev->rx_buf[dev->rx_tail];
                    uint16_t payload_len = flen - (uint16_t)hdr_len - 8;

                    /* Clamp to buffer size (14-byte ETH header + payload) */
                    if (14 + payload_len > IWN_RX_BUF_SIZE)
                        payload_len = IWN_RX_BUF_SIZE - 14;

                    /* ETH DST = Addr1 (us) */
                    iwn_memcpy(eth, frame + 4, 6);
                    /* ETH SRC = Addr3 (original source) */
                    iwn_memcpy(eth + 6, frame + 16, 6);
                    /* EtherType from LLC/SNAP */
                    eth[12] = snap[6];
                    eth[13] = snap[7];
                    /* Payload (after LLC/SNAP) */
                    iwn_memcpy(eth + 14, snap + 8, payload_len);

                    dev->rx_buf_len[dev->rx_tail] = 14 + payload_len;
                    dev->rx_tail = next_tail;
                    queued++;
                }
            }
        } else if (desc->type == IWN_TX_DONE) {
            /* TX completion -- update counters */
            dev->tx_ok++;
        }
        /* Other notification types: silently ignore */

advance:
        dev->rxq.cur = (dev->rxq.cur + 1) % IWN_RX_RING_COUNT;
    }

    /* Update WPTR and clear interrupt status */
    iwn_write(dev, IWN_FH_RX_WPTR,
        ((dev->rxq.cur + IWN_RX_RING_COUNT - 1) %
         IWN_RX_RING_COUNT) & ~7);
    iwn_write(dev, IWN_INT, 0xffffffff);
    iwn_write(dev, IWN_FH_INT, 0xffffffff);

    return queued;
}

/* =====================================================================
 * iwn_rx_pending -- Count of queued frames in software buffer
 * ===================================================================== */

int iwn_rx_pending(iwn_dev *dev) {
    return (dev->rx_tail - dev->rx_head + IWN_RX_BUF_COUNT) %
            IWN_RX_BUF_COUNT;
}

/* =====================================================================
 * iwn_rx_dequeue -- Dequeue one Ethernet frame from the software buffer
 *
 * Calls iwn_rx_poll() to drain any new HW frames first.
 * Returns 0 on success, -1 if no frames available.
 * ===================================================================== */

int iwn_rx_dequeue(iwn_dev *dev, uint8_t *buf, uint16_t *buf_len) {
    /* Drain any new frames from hardware */
    iwn_rx_poll(dev);

    /* Check if anything is queued */
    if (dev->rx_head == dev->rx_tail)
        return -1;

    /* Copy frame out */
    uint16_t len = dev->rx_buf_len[dev->rx_head];
    iwn_memcpy(buf, dev->rx_buf[dev->rx_head], len);
    *buf_len = len;

    /* Advance consumer */
    dev->rx_head = (dev->rx_head + 1) % IWN_RX_BUF_COUNT;
    return 0;
}

/* =====================================================================
 * iwn_ping_cmd -- ARP ping smoke test (REPL command)
 *
 * Sends an ARP request for 192.168.1.1 (gateway) via broadcast,
 * polls for ARP reply for 3 seconds.
 * ===================================================================== */

void iwn_ping_cmd(void) {
    if (!g_iwn_found) {
        efi_print_ascii("  iwn: not initialized\r\n");
        return;
    }

    iwn_dev *dev = &g_iwn;
    if (dev->state != IWN_STATE_RUN || !dev->keys_installed) {
        efi_print_ascii("  iwn: not connected (run iwn connect first)\r\n");
        return;
    }

    efi_print_ascii("  Sending ARP request for 192.168.1.1...\r\n");

    /* Build ARP request payload (28 bytes) */
    uint8_t arp[28];
    iwn_memset(arp, 0, sizeof(arp));

    /* Hardware type: Ethernet (1) */
    arp[0] = 0x00; arp[1] = 0x01;
    /* Protocol type: IPv4 (0x0800) */
    arp[2] = 0x08; arp[3] = 0x00;
    /* Hardware size: 6 */
    arp[4] = 0x06;
    /* Protocol size: 4 */
    arp[5] = 0x04;
    /* Opcode: request (1) */
    arp[6] = 0x00; arp[7] = 0x01;

    /* Sender MAC: our MAC */
    iwn_memcpy(arp + 8, dev->mac_addr, 6);
    /* Sender IP: 0.0.0.0 (we don't have an IP yet -- gratuitous style) */
    arp[14] = 0; arp[15] = 0; arp[16] = 0; arp[17] = 0;
    /* Target MAC: 00:00:00:00:00:00 (unknown) */
    /* Target IP: 192.168.1.1 */
    arp[24] = 192; arp[25] = 168; arp[26] = 1; arp[27] = 1;

    /* Send as broadcast (ethertype 0x0806 = ARP) */
    uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    if (iwn_encap_tx(dev, bcast, 0x0806, arp, 28) != 0) {
        efi_print_ascii("  ARP TX failed\r\n");
        return;
    }

    efi_print_ascii("  ARP sent, waiting for reply...\r\n");

    /* Poll for ARP reply for 3 seconds */
    uint8_t rx_frame[1518];
    uint16_t rx_len;

    for (int t = 0; t < 3000; t++) {
        if (iwn_rx_dequeue(dev, rx_frame, &rx_len) == 0) {
            /* Check if it's an ARP reply (ethertype 0x0806) */
            if (rx_len >= 42 &&
                rx_frame[12] == 0x08 && rx_frame[13] == 0x06) {
                /* ARP opcode at offset 20-21 in Ethernet frame
                 * (14-byte ETH header + 6 bytes into ARP) */
                uint16_t opcode = ((uint16_t)rx_frame[20] << 8) |
                                  rx_frame[21];
                if (opcode == 2) {
                    /* ARP reply! Print sender IP and MAC */
                    efi_print_ascii("  ARP reply from ");
                    print_dec((uint32_t)rx_frame[28]);
                    efi_print_ascii(".");
                    print_dec((uint32_t)rx_frame[29]);
                    efi_print_ascii(".");
                    print_dec((uint32_t)rx_frame[30]);
                    efi_print_ascii(".");
                    print_dec((uint32_t)rx_frame[31]);
                    efi_print_ascii(" [");
                    for (int i = 0; i < 6; i++) {
                        print_hex8(rx_frame[22 + i]);
                        if (i < 5) efi_print_ascii(":");
                    }
                    efi_print_ascii("]\r\n");
                    efi_print_ascii("  Data path working!\r\n");
                    return;
                }
            }

            /* Not an ARP reply -- print what we got */
            efi_print_ascii("  RX: ethertype 0x");
            print_hex8(rx_frame[12]);
            print_hex8(rx_frame[13]);
            efi_print_ascii(" len=");
            print_dec((uint32_t)rx_len);
            efi_print_ascii("\r\n");
        }

        gBS->Stall(1000);  /* 1 ms */
    }

    efi_print_ascii("  No ARP reply (timeout 3s)\r\n");
}
