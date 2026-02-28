/*
 * iwn_eapol.c — WPA2-PSK EAPOL 4-Way Handshake for Intel Wireless N
 *
 * Phase 4c: After association (Phase 4b), the AP initiates the EAPOL
 * 4-way handshake. We derive PMK from the password, receive Message 1
 * (ANonce), derive PTK, send Message 2 (SNonce + MIC), receive and
 * verify Message 3, send Message 4, then install PTK + GTK into the
 * firmware for encrypted data frames.
 *
 * EAPOL travels inside 802.11 data frames (type=2) with LLC/SNAP
 * encapsulation (ethertype 0x888E), not management frames.
 *
 * Reference: IEEE 802.11i-2004, RFC 2898, RFC 3394
 */

#include "iwn_internal.h"
#include "iwn_crypto.h"

/* ═══════════════════════════════════════════════════════════════════════
 * EAPOL-Key body field offsets (after 4-byte EAPOL header)
 * ═══════════════════════════════════════════════════════════════════════ */

#define EAPOL_KEY_DESC_TYPE    0    /* 1 byte: 0x02 for WPA2 */
#define EAPOL_KEY_INFO         1    /* 2 bytes, big-endian */
#define EAPOL_KEY_LEN          3    /* 2 bytes */
#define EAPOL_KEY_REPLAY       5    /* 8 bytes */
#define EAPOL_KEY_NONCE        13   /* 32 bytes */
#define EAPOL_KEY_IV           45   /* 16 bytes */
#define EAPOL_KEY_RSC          61   /* 8 bytes */
#define EAPOL_KEY_RESERVED     69   /* 8 bytes */
#define EAPOL_KEY_MIC          77   /* 16 bytes */
#define EAPOL_KEY_DATA_LEN     93   /* 2 bytes, big-endian */
#define EAPOL_KEY_DATA         95   /* variable */
#define EAPOL_KEY_BODY_MIN     95   /* minimum body size (no key data) */

/* Key Info bit fields */
#define EAPOL_KI_VERSION_MASK  0x0007
#define EAPOL_KI_PAIRWISE      (1 << 3)
#define EAPOL_KI_INSTALL       (1 << 6)
#define EAPOL_KI_ACK           (1 << 7)
#define EAPOL_KI_MIC           (1 << 8)
#define EAPOL_KI_SECURE        (1 << 9)
#define EAPOL_KI_ENCRYPTED     (1 << 12)

/* 802.11 data frame + LLC/SNAP overhead before EAPOL */
#define DOT11_DATA_HDR_LEN  24
#define LLC_SNAP_LEN         8

/* EAPOL header size (version + type + body_length) */
#define EAPOL_HDR_LEN        4

/* ═══════════════════════════════════════════════════════════════════════
 * Static helpers
 * ═══════════════════════════════════════════════════════════════════════ */

static int eapol_memcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (uint32_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

static int eapol_strlen(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

/* Big-endian 16-bit load/store */
static uint16_t be16_load(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static void be16_store(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

/* ═══════════════════════════════════════════════════════════════════════
 * SNonce generation — RDTSC + MAC + SHA-1
 * ═══════════════════════════════════════════════════════════════════════ */

static uint32_t snonce_counter = 0;

static void iwn_generate_snonce(iwn_dev *dev, uint8_t snonce[32]) {
    /* First 20 bytes: SHA-1(TSC || MAC || counter) */
    {
        uint8_t buf[22]; /* 8 (TSC) + 6 (MAC) + 4 (counter) + 4 (TSC low) */
        uint64_t tsc = __builtin_ia32_rdtsc();
        for (int i = 0; i < 8; i++)
            buf[i] = (uint8_t)(tsc >> (i * 8));
        iwn_memcpy(buf + 8, dev->mac_addr, 6);
        snonce_counter++;
        buf[14] = (uint8_t)(snonce_counter);
        buf[15] = (uint8_t)(snonce_counter >> 8);
        buf[16] = (uint8_t)(snonce_counter >> 16);
        buf[17] = (uint8_t)(snonce_counter >> 24);
        /* Re-read TSC for more entropy */
        tsc = __builtin_ia32_rdtsc();
        buf[18] = (uint8_t)(tsc);
        buf[19] = (uint8_t)(tsc >> 8);
        buf[20] = (uint8_t)(tsc >> 16);
        buf[21] = (uint8_t)(tsc >> 24);

        iwn_sha1(buf, 22, snonce);
    }

    /* Next 12 bytes: SHA-1(counter+1 || TSC) truncated */
    {
        uint8_t buf[12];
        snonce_counter++;
        buf[0] = (uint8_t)(snonce_counter);
        buf[1] = (uint8_t)(snonce_counter >> 8);
        buf[2] = (uint8_t)(snonce_counter >> 16);
        buf[3] = (uint8_t)(snonce_counter >> 24);
        uint64_t tsc = __builtin_ia32_rdtsc();
        for (int i = 0; i < 8; i++)
            buf[4 + i] = (uint8_t)(tsc >> (i * 8));

        uint8_t hash[20];
        iwn_sha1(buf, 12, hash);
        iwn_memcpy(snonce + 20, hash, 12);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * PTK derivation — PRF-384(PMK, "Pairwise key expansion", data)
 * ═══════════════════════════════════════════════════════════════════════ */

static void iwn_derive_ptk(iwn_dev *dev, const uint8_t pmk[32],
                            const uint8_t anonce[32],
                            const uint8_t snonce[32],
                            uint8_t ptk[48]) {
    uint8_t data[76]; /* min(AA,SPA) || max(AA,SPA) || min(AN,SN) || max(AN,SN) */

    /* AA = AP MAC (target_bssid), SPA = our MAC (mac_addr) */
    const uint8_t *aa = dev->target_bssid;
    const uint8_t *spa = dev->mac_addr;

    /* Lexicographic ordering of MAC addresses */
    if (eapol_memcmp(aa, spa, 6) < 0) {
        iwn_memcpy(data, aa, 6);
        iwn_memcpy(data + 6, spa, 6);
    } else {
        iwn_memcpy(data, spa, 6);
        iwn_memcpy(data + 6, aa, 6);
    }

    /* Lexicographic ordering of nonces */
    if (eapol_memcmp(anonce, snonce, 32) < 0) {
        iwn_memcpy(data + 12, anonce, 32);
        iwn_memcpy(data + 44, snonce, 32);
    } else {
        iwn_memcpy(data + 12, snonce, 32);
        iwn_memcpy(data + 44, anonce, 32);
    }

    static const uint8_t label[] = "Pairwise key expansion";
    iwn_prf384(pmk, label, 22, data, 76, ptk);
}

/* ═══════════════════════════════════════════════════════════════════════
 * EAPOL MIC computation — HMAC-SHA1(KCK, eapol_frame)[0:16]
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Compute MIC over an EAPOL frame. The MIC covers the EAPOL header (4 bytes)
 * + EAPOL-Key body, with the MIC field zeroed.
 *
 * eapol_frame: points to EAPOL header (version, type, body_length)
 * eapol_len: total length of EAPOL header + body
 * mic_offset: offset of MIC field within eapol_frame
 */
static void iwn_eapol_compute_mic(const uint8_t *kck,
                                   uint8_t *eapol_frame,
                                   uint16_t eapol_len,
                                   uint16_t mic_offset,
                                   uint8_t mic_out[16]) {
    /* Save and zero MIC field */
    uint8_t saved_mic[16];
    iwn_memcpy(saved_mic, eapol_frame + mic_offset, 16);
    iwn_memset(eapol_frame + mic_offset, 0, 16);

    /* HMAC-SHA1(KCK, eapol_frame) */
    uint8_t hmac[20];
    iwn_hmac_sha1(kck, 16, eapol_frame, eapol_len, hmac);

    /* Take first 16 bytes */
    iwn_memcpy(mic_out, hmac, 16);

    /* Restore original MIC field */
    iwn_memcpy(eapol_frame + mic_offset, saved_mic, 16);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Poll RX ring for EAPOL data frames
 * ═══════════════════════════════════════════════════════════════════════ */

/* LLC/SNAP header for 802.1X authentication */
static const uint8_t llc_snap_eapol[8] = {
    0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E
};

/*
 * Poll for an EAPOL data frame. Returns 0 on success.
 * On success, eapol_out points to the EAPOL header (after LLC/SNAP),
 * eapol_len_out is the EAPOL header + body length.
 * Also copies the raw key RSC from M3 into rsc_out (8 bytes) if non-NULL.
 */
static int iwn_poll_eapol(iwn_dev *dev,
                           uint8_t *eapol_out, uint16_t *eapol_len_out,
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

                    /* Check for deauth (type=0 mgmt, subtype=12) */
                    if (type == 0 && subtype == 12) {
                        efi_print_ascii("  EAPOL: AP sent deauth!\r\n");
                        dev->rxq.cur = (dev->rxq.cur + 1) %
                                       IWN_RX_RING_COUNT;
                        iwn_write(dev, IWN_FH_RX_WPTR,
                            ((dev->rxq.cur + IWN_RX_RING_COUNT - 1) %
                             IWN_RX_RING_COUNT) & ~7);
                        return -1;
                    }

                    /* Data frame: type=2, subtype=0 */
                    if (type == 2 && (subtype == 0 || subtype == 8)) {
                        /* Check LLC/SNAP header for 802.1X */
                        if (flen >= DOT11_DATA_HDR_LEN + LLC_SNAP_LEN &&
                            eapol_memcmp(frame + DOT11_DATA_HDR_LEN,
                                         llc_snap_eapol, 8) == 0) {
                            /* Found EAPOL frame */
                            uint16_t eapol_start =
                                DOT11_DATA_HDR_LEN + LLC_SNAP_LEN;
                            uint16_t eapol_len = flen - eapol_start;

                            if (eapol_len > 4 && eapol_out) {
                                if (eapol_len > 512) eapol_len = 512;
                                iwn_memcpy(eapol_out,
                                           frame + eapol_start, eapol_len);
                            }
                            if (eapol_len_out)
                                *eapol_len_out = eapol_len;

                            dev->rxq.cur = (dev->rxq.cur + 1) %
                                           IWN_RX_RING_COUNT;
                            iwn_write(dev, IWN_FH_RX_WPTR,
                                ((dev->rxq.cur + IWN_RX_RING_COUNT - 1) %
                                 IWN_RX_RING_COUNT) & ~7);
                            return 0;
                        }
                    }
                }
            } else if (desc->type == IWN_TX_DONE) {
                /* TX completion — expected, silently consume */
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

/* ═══════════════════════════════════════════════════════════════════════
 * Transmit an EAPOL data frame
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Build and send an 802.11 data frame containing an EAPOL-Key message.
 * eapol_body: EAPOL-Key body (descriptor type through key data)
 * eapol_body_len: length of EAPOL-Key body
 */
static int iwn_tx_eapol(iwn_dev *dev,
                         const uint8_t *eapol_body, uint16_t eapol_body_len) {
    /* Total frame: 802.11 hdr (24) + LLC/SNAP (8) + EAPOL hdr (4) + body */
    uint16_t frame_len = DOT11_DATA_HDR_LEN + LLC_SNAP_LEN +
                         EAPOL_HDR_LEN + eapol_body_len;
    uint8_t frame[512];
    iwn_memset(frame, 0, sizeof(frame));

    if (frame_len > sizeof(frame))
        return -1;

    /* 802.11 Data header */
    frame[0] = 0x08;  /* FC byte 0: type=2 (data), subtype=0 */
    frame[1] = 0x01;  /* FC byte 1: ToDS=1 */
    /* Duration: 0 */
    /* Addr1 (RA/BSSID) = AP */
    iwn_memcpy(frame + 4, dev->target_bssid, 6);
    /* Addr2 (TA/SA) = our MAC */
    iwn_memcpy(frame + 10, dev->mac_addr, 6);
    /* Addr3 (DA) = AP */
    iwn_memcpy(frame + 16, dev->target_bssid, 6);
    /* SeqCtrl = 0 (firmware fills via AUTO_SEQ) */

    /* LLC/SNAP header */
    iwn_memcpy(frame + DOT11_DATA_HDR_LEN, llc_snap_eapol, 8);

    /* EAPOL header */
    uint8_t *eapol_hdr = frame + DOT11_DATA_HDR_LEN + LLC_SNAP_LEN;
    eapol_hdr[0] = 0x01;  /* Version: 802.1X-2001 */
    eapol_hdr[1] = 0x03;  /* Type: EAPOL-Key */
    be16_store(eapol_hdr + 2, eapol_body_len);

    /* EAPOL-Key body */
    iwn_memcpy(eapol_hdr + EAPOL_HDR_LEN, eapol_body, eapol_body_len);

    /* Send via iwn_tx_raw() — handles 2-segment DMA for large frames.
     * CCK 1 Mbps for EAPOL reliability. Unencrypted (security=0). */
    uint8_t txant = (uint8_t)IWN_LSB(dev->txchainmask);
    return iwn_tx_raw(dev, frame, frame_len,
                      IWN_ID_BSS, 0,
                      10, IWN_RFLAG_CCK | IWN_RFLAG_ANT(txant));
}

/* ═══════════════════════════════════════════════════════════════════════
 * Build EAPOL-Key body helper
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Build an EAPOL-Key body. Returns total body length.
 * body must be at least EAPOL_KEY_BODY_MIN + key_data_len bytes.
 */
static uint16_t build_eapol_key_body(uint8_t *body,
                                      uint16_t key_info,
                                      const uint8_t replay[8],
                                      const uint8_t nonce[32],
                                      const uint8_t *key_data,
                                      uint16_t key_data_len) {
    iwn_memset(body, 0, EAPOL_KEY_BODY_MIN + key_data_len);

    /* Descriptor Type: RSN (WPA2) */
    body[EAPOL_KEY_DESC_TYPE] = 0x02;

    /* Key Info (big-endian) */
    be16_store(body + EAPOL_KEY_INFO, key_info);

    /* Key Length: 16 for CCMP */
    be16_store(body + EAPOL_KEY_LEN, 0x0010);

    /* Replay Counter */
    if (replay)
        iwn_memcpy(body + EAPOL_KEY_REPLAY, replay, 8);

    /* Key Nonce */
    if (nonce)
        iwn_memcpy(body + EAPOL_KEY_NONCE, nonce, 32);

    /* Key Data */
    be16_store(body + EAPOL_KEY_DATA_LEN, key_data_len);
    if (key_data && key_data_len > 0)
        iwn_memcpy(body + EAPOL_KEY_DATA, key_data, key_data_len);

    return EAPOL_KEY_BODY_MIN + key_data_len;
}

/* ═══════════════════════════════════════════════════════════════════════
 * Compute and write MIC into an EAPOL-Key frame
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Given a fully constructed EAPOL-Key body, build the complete EAPOL frame
 * (header + body), compute MIC, and write it into the body's MIC field.
 */
static void sign_eapol_body(const uint8_t *kck,
                             uint8_t *body, uint16_t body_len) {
    /* Build temporary EAPOL frame for MIC computation */
    uint8_t eapol_frame[512];
    eapol_frame[0] = 0x01;  /* Version */
    eapol_frame[1] = 0x03;  /* Type: EAPOL-Key */
    be16_store(eapol_frame + 2, body_len);
    iwn_memcpy(eapol_frame + EAPOL_HDR_LEN, body, body_len);

    uint16_t eapol_len = EAPOL_HDR_LEN + body_len;
    uint16_t mic_offset = EAPOL_HDR_LEN + EAPOL_KEY_MIC;

    uint8_t mic[16];
    iwn_eapol_compute_mic(kck, eapol_frame, eapol_len, mic_offset, mic);

    /* Write MIC into the body */
    iwn_memcpy(body + EAPOL_KEY_MIC, mic, 16);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Parse GTK KDE from M3 Key Data
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Walk TLV entries in decrypted key data to find GTK KDE.
 * Returns 0 on success, fills gtk_out (16 bytes) and kid_out.
 */
static int parse_gtk_kde(const uint8_t *key_data, uint16_t key_data_len,
                          uint8_t gtk_out[16], uint8_t *kid_out) {
    uint16_t pos = 0;

    while (pos + 2 <= key_data_len) {
        uint8_t tag = key_data[pos];
        uint8_t len = key_data[pos + 1];

        if (pos + 2 + len > key_data_len)
            break;

        /* GTK KDE: tag=0xDD, OUI=00:0F:AC, data type=0x01 */
        if (tag == 0xDD && len >= 6 + 16) {
            const uint8_t *kde = key_data + pos + 2;
            if (kde[0] == 0x00 && kde[1] == 0x0F &&
                kde[2] == 0xAC && kde[3] == 0x01) {
                /* KDE[4] = reserved | key ID (bits 0-1) */
                *kid_out = kde[4] & 0x03;
                /* KDE[6..21] = GTK (after 2 bytes of key ID + TX) */
                iwn_memcpy(gtk_out, kde + 6, 16);
                return 0;
            }
        }

        pos += 2 + len;
    }

    return -1;  /* GTK KDE not found */
}

/* ═══════════════════════════════════════════════════════════════════════
 * Install pairwise + group keys into firmware
 * ═══════════════════════════════════════════════════════════════════════ */

static int iwn_install_keys(iwn_dev *dev, const uint8_t *tk,
                             const uint8_t *gtk, uint8_t gtk_kid,
                             const uint8_t *gtk_rsc) {
    /* 1. Install pairwise key (TK) on AP node (id=0) */
    efi_print_ascii("  Installing pairwise key...\r\n");
    {
        struct iwn_node_info node;
        iwn_memset(&node, 0, sizeof(node));
        node.id = IWN_ID_BSS;
        node.control = IWN_NODE_UPDATE;
        node.flags = IWN_FLAG_SET_KEY;
        iwn_memcpy(node.macaddr, dev->target_bssid, 6);
        node.kflags = htole16(IWN_KFLAG_CCMP | IWN_KFLAG_MAP | IWN_KFLAG_KID(0));
        iwn_memcpy(node.key, tk, 16);

        /* Debug: print kflags and first 4 key bytes */
        {
            uint16_t kf = IWN_KFLAG_CCMP | IWN_KFLAG_MAP | IWN_KFLAG_KID(0);
            efi_print_ascii("    kflags=0x");
            print_hex8((uint8_t)(kf >> 8));
            print_hex8((uint8_t)(kf));
            efi_print_ascii(" key[0:4]=");
            print_hex8(tk[0]); print_hex8(tk[1]);
            print_hex8(tk[2]); print_hex8(tk[3]);
            efi_print_ascii("\r\n");
        }
        node.tsc = 0;

        if (iwn_cmd(dev, IWN_CMD_ADD_NODE, &node, sizeof(node)) != 0) {
            efi_print_ascii("  EAPOL: pairwise key install failed\r\n");
            return -1;
        }
        if (iwn_wait_cmd(dev, IWN_CMD_ADD_NODE,
                         "ADD_NODE(pairwise)", 200) != 0)
            return -1;
    }

    /* 2. Install group key (GTK) on broadcast node (id=15) */
    efi_print_ascii("  Installing group key...\r\n");
    {
        struct iwn_node_info node;
        iwn_memset(&node, 0, sizeof(node));
        node.id = IWN5000_ID_BROADCAST;
        node.control = IWN_NODE_UPDATE;
        node.flags = IWN_FLAG_SET_KEY;
        iwn_memset(node.macaddr, 0xff, 6);
        node.kflags = htole16(IWN_KFLAG_CCMP |
                              IWN_KFLAG_KID(gtk_kid) |
                              IWN_KFLAG_GROUP);

        /* Debug: print kflags and kid */
        {
            uint16_t kf = IWN_KFLAG_CCMP |
                          IWN_KFLAG_KID(gtk_kid) | IWN_KFLAG_GROUP;
            efi_print_ascii("    kflags=0x");
            print_hex8((uint8_t)(kf >> 8));
            print_hex8((uint8_t)(kf));
            efi_print_ascii(" kid=");
            print_dec((uint32_t)gtk_kid);
            efi_print_ascii(" key[0:4]=");
            print_hex8(gtk[0]); print_hex8(gtk[1]);
            print_hex8(gtk[2]); print_hex8(gtk[3]);
            efi_print_ascii("\r\n");
        }
        iwn_memcpy(node.key, gtk, 16);

        /* Set RSC (replay counter) from M3 if provided */
        if (gtk_rsc) {
            uint64_t rsc = 0;
            for (int i = 0; i < 8; i++)
                rsc |= (uint64_t)gtk_rsc[i] << (i * 8);
            node.tsc = rsc;
        }

        if (iwn_cmd(dev, IWN_CMD_ADD_NODE, &node, sizeof(node)) != 0) {
            efi_print_ascii("  EAPOL: group key install failed\r\n");
            return -1;
        }
        if (iwn_wait_cmd(dev, IWN_CMD_ADD_NODE,
                         "ADD_NODE(group)", 200) != 0)
            return -1;
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * Main 4-Way Handshake
 * ═══════════════════════════════════════════════════════════════════════ */

int iwn_eapol_handshake(iwn_dev *dev) {
    uint8_t pmk[32];
    uint8_t anonce[32];
    uint8_t snonce[32];
    uint8_t ptk[48];
    uint8_t replay_m1[8];
    uint8_t replay_m3[8];
    uint8_t eapol_buf[512];
    uint16_t eapol_len;

    efi_print_ascii("  EAPOL: starting 4-way handshake\r\n");

    /* ──────────────────────────────────────────────────────────────────
     * Step 1: Derive PMK from password + SSID
     * PBKDF2-SHA1 with 4096 iterations (~0.5s on x86)
     * ────────────────────────────────────────────────────────────────── */
    {
        int pw_len = eapol_strlen(dev->password);
        if (pw_len == 0) {
            efi_print_ascii("  EAPOL: no password set!\r\n");
            return -1;
        }

        efi_print_ascii("  Deriving PMK (PBKDF2, 4096 rounds)...\r\n");
        iwn_pbkdf2_sha1((const uint8_t *)dev->password, (size_t)pw_len,
                        (const uint8_t *)dev->target_ssid,
                        (size_t)dev->target_ssid_len,
                        4096, pmk, 32);
        efi_print_ascii("  PMK derived\r\n");
    }

    /* ──────────────────────────────────────────────────────────────────
     * Step 2: Receive Message 1 (AP → STA)
     * Contains ANonce. Key Info: ACK set, MIC not set.
     * ────────────────────────────────────────────────────────────────── */
    efi_print_ascii("  Waiting for M1...\r\n");
    {
        if (iwn_poll_eapol(dev, eapol_buf, &eapol_len, 5000) != 0) {
            efi_print_ascii("  EAPOL: M1 timeout\r\n");
            return -1;
        }

        /* eapol_buf[0..3] = EAPOL header, [4+] = EAPOL-Key body */
        if (eapol_len < EAPOL_HDR_LEN + EAPOL_KEY_BODY_MIN) {
            efi_print_ascii("  EAPOL: M1 too short\r\n");
            return -1;
        }

        uint8_t *body = eapol_buf + EAPOL_HDR_LEN;

        /* Validate: descriptor type == 0x02 (RSN) */
        if (body[EAPOL_KEY_DESC_TYPE] != 0x02) {
            efi_print_ascii("  EAPOL: M1 bad descriptor type\r\n");
            return -1;
        }

        /* Validate key info: ACK set, MIC not set */
        uint16_t ki = be16_load(body + EAPOL_KEY_INFO);
        if (!(ki & EAPOL_KI_ACK) || (ki & EAPOL_KI_MIC)) {
            efi_print_ascii("  EAPOL: M1 unexpected key info 0x");
            print_hex8((uint8_t)(ki >> 8));
            print_hex8((uint8_t)(ki));
            efi_print_ascii("\r\n");
            return -1;
        }

        /* Extract ANonce and replay counter */
        iwn_memcpy(anonce, body + EAPOL_KEY_NONCE, 32);
        iwn_memcpy(replay_m1, body + EAPOL_KEY_REPLAY, 8);

        efi_print_ascii("  M1 received (ANonce ");
        print_hex8(anonce[0]); print_hex8(anonce[1]);
        efi_print_ascii("...)\r\n");
    }

    /* ──────────────────────────────────────────────────────────────────
     * Step 3: Generate SNonce and derive PTK
     * PTK = PRF-384(PMK, "Pairwise key expansion", data)
     * KCK = PTK[0:16], KEK = PTK[16:32], TK = PTK[32:48]
     * ────────────────────────────────────────────────────────────────── */
    iwn_generate_snonce(dev, snonce);
    iwn_derive_ptk(dev, pmk, anonce, snonce, ptk);

    uint8_t *kck = ptk;         /* Key Confirmation Key (MIC) */
    uint8_t *kek = ptk + 16;    /* Key Encryption Key (unwrap) */
    uint8_t *tk  = ptk + 32;    /* Temporal Key (data encryption) */

    efi_print_ascii("  PTK derived\r\n");

    /* ──────────────────────────────────────────────────────────────────
     * Step 4: Build and send Message 2 (STA → AP)
     * Contains SNonce + RSN IE + MIC
     * Key Info: 0x010A (VERSION=2, PAIRWISE=1, MIC=1)
     * ────────────────────────────────────────────────────────────────── */
    {
        uint8_t body[256];
        uint16_t body_len = build_eapol_key_body(
            body,
            0x010A,         /* VERSION=2, PAIRWISE, MIC */
            replay_m1,      /* echo M1 replay counter */
            snonce,         /* our nonce */
            dev->rsn_ie,    /* RSN IE from association */
            dev->rsn_ie_len
        );

        /* Compute and write MIC */
        sign_eapol_body(kck, body, body_len);

        efi_print_ascii("  Sending M2...\r\n");
        if (iwn_tx_eapol(dev, body, body_len) != 0) {
            efi_print_ascii("  EAPOL: M2 TX failed\r\n");
            return -1;
        }
        efi_print_ascii("  M2 sent\r\n");
    }

    /* ──────────────────────────────────────────────────────────────────
     * Step 5: Receive Message 3 (AP → STA)
     * Verify MIC, decrypt key data, extract GTK
     * Key Info should have: INSTALL+ACK+MIC+SECURE+ENCRYPTED
     * ────────────────────────────────────────────────────────────────── */
    efi_print_ascii("  Waiting for M3...\r\n");
    {
        /* Loop to handle M1 retransmissions from AP.
         * The AP may resend M1 if it didn't receive M2 in time.
         * We ignore M1 retransmissions and keep waiting for M3. */
        int m3_attempts = 0;
    wait_m3:
        if (iwn_poll_eapol(dev, eapol_buf, &eapol_len, 5000) != 0) {
            efi_print_ascii("  EAPOL: M3 timeout\r\n");
            return -1;
        }

        if (eapol_len < EAPOL_HDR_LEN + EAPOL_KEY_BODY_MIN) {
            efi_print_ascii("  EAPOL: M3 too short\r\n");
            return -1;
        }

        uint8_t *body = eapol_buf + EAPOL_HDR_LEN;

        /* Validate descriptor type */
        if (body[EAPOL_KEY_DESC_TYPE] != 0x02) {
            efi_print_ascii("  EAPOL: M3 bad descriptor type\r\n");
            return -1;
        }

        /* Validate key info: INSTALL+ACK+MIC+SECURE+ENCRYPTED */
        uint16_t ki = be16_load(body + EAPOL_KEY_INFO);
        if (!(ki & EAPOL_KI_ACK) || !(ki & EAPOL_KI_MIC) ||
            !(ki & EAPOL_KI_SECURE)) {
            /* M1 retransmission: has ACK but no MIC/SECURE.
             * Ignore and keep waiting for real M3 (max 3 retries). */
            if (++m3_attempts < 4) {
                efi_print_ascii("  EAPOL: M1 retransmission, ignoring\r\n");
                goto wait_m3;
            }
            efi_print_ascii("  EAPOL: M3 unexpected key info 0x");
            print_hex8((uint8_t)(ki >> 8));
            print_hex8((uint8_t)(ki));
            efi_print_ascii("\r\n");
            return -1;
        }

        /* Verify MIC */
        uint8_t received_mic[16];
        iwn_memcpy(received_mic, body + EAPOL_KEY_MIC, 16);

        uint8_t computed_mic[16];
        /* MIC offset in the full EAPOL frame (header + body) */
        iwn_eapol_compute_mic(kck, eapol_buf, eapol_len,
                              EAPOL_HDR_LEN + EAPOL_KEY_MIC,
                              computed_mic);

        if (eapol_memcmp(received_mic, computed_mic, 16) != 0) {
            efi_print_ascii("  EAPOL: M3 MIC verification failed!\r\n");
            return -1;
        }
        efi_print_ascii("  M3 MIC verified\r\n");

        /* Save replay counter from M3 */
        iwn_memcpy(replay_m3, body + EAPOL_KEY_REPLAY, 8);

        /* Extract and decrypt key data */
        uint16_t kd_len = be16_load(body + EAPOL_KEY_DATA_LEN);
        if (kd_len < 16 || kd_len > 256 ||
            EAPOL_HDR_LEN + EAPOL_KEY_DATA + kd_len > eapol_len) {
            efi_print_ascii("  EAPOL: M3 key data length invalid\r\n");
            return -1;
        }

        uint8_t *enc_kd = body + EAPOL_KEY_DATA;
        uint8_t plain_kd[256];

        if (iwn_aes_unwrap(kek, enc_kd, kd_len, plain_kd) != 0) {
            efi_print_ascii("  EAPOL: AES unwrap failed!\r\n");
            return -1;
        }
        efi_print_ascii("  Key data decrypted\r\n");

        /* Parse GTK KDE from decrypted key data */
        uint16_t plain_kd_len = kd_len - 8; /* unwrap removes 8-byte IV */
        uint8_t gtk[16];
        uint8_t gtk_kid;

        if (parse_gtk_kde(plain_kd, plain_kd_len, gtk, &gtk_kid) != 0) {
            efi_print_ascii("  EAPOL: GTK KDE not found!\r\n");
            return -1;
        }

        efi_print_ascii("  GTK extracted (kid=");
        print_dec((uint32_t)gtk_kid);
        efi_print_ascii(")\r\n");

        /* Save GTK into device state */
        iwn_memcpy(dev->gtk, gtk, 16);
        dev->gtk_kid = gtk_kid;

        /* Save RSC from M3 for group key */
        uint8_t gtk_rsc[8];
        iwn_memcpy(gtk_rsc, body + EAPOL_KEY_RSC, 8);

        /* ──────────────────────────────────────────────────────────────
         * Step 6: Build and send Message 4 (STA → AP)
         * MIC only, no key data
         * Key Info: 0x030A (VERSION=2, PAIRWISE=1, MIC=1, SECURE=1)
         * ────────────────────────────────────────────────────────────── */
        {
            uint8_t m4_body[256];
            uint16_t m4_body_len = build_eapol_key_body(
                m4_body,
                0x030A,         /* VERSION=2, PAIRWISE, MIC, SECURE */
                replay_m3,      /* echo M3 replay counter */
                NULL,           /* no nonce in M4 */
                NULL, 0         /* no key data */
            );

            /* Compute and write MIC */
            sign_eapol_body(kck, m4_body, m4_body_len);

            efi_print_ascii("  Sending M4...\r\n");
            if (iwn_tx_eapol(dev, m4_body, m4_body_len) != 0) {
                efi_print_ascii("  EAPOL: M4 TX failed\r\n");
                return -1;
            }
            efi_print_ascii("  M4 sent\r\n");

            /* Wait for M4 to actually transmit before installing keys.
             * Without this delay, the AP may not have received M4 yet
             * and will deauth us when we start sending encrypted data. */
            gBS->Stall(200000);  /* 200 ms */
        }

        /* ──────────────────────────────────────────────────────────────
         * Step 7: Install keys into firmware
         * ────────────────────────────────────────────────────────────── */
        if (iwn_install_keys(dev, tk, gtk, gtk_kid, gtk_rsc) != 0)
            return -1;

        /* Save PTK into device state */
        iwn_memcpy(dev->ptk, ptk, 48);
        dev->keys_installed = 1;
    }

    efi_print_ascii("  EAPOL: 4-way handshake complete!\r\n");
    return 0;
}
