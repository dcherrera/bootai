/*
 * iwn_ccmp.c — Software CCMP (AES-CCM) decryption for 802.11 data frames
 *
 * The Intel 6000 firmware accepts key install commands but never populates
 * the HW key lookup table (MPDU tail flags show SEC_ENC_ERR on every frame).
 * With NODECRYPT set, encrypted frames arrive raw. This module decrypts
 * them in software using the PTK TK or GTK stored in dev->ptk/dev->gtk.
 *
 * Only iwn_aes128_encrypt() is used — CCM uses AES encrypt for both the
 * CTR keystream and the CBC-MAC. AES decrypt is not needed.
 *
 * Reference: IEEE 802.11i-2004, RFC 3610 (AES-CCM)
 */

#include "iwn_internal.h"
#include "iwn_crypto.h"

/* ── Helpers (no libc) ─────────────────────────────────────────────── */

static void ccmp_memcpy(void *dst, const void *src, int n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

static void ccmp_memset(void *dst, uint8_t val, int n) {
    uint8_t *d = (uint8_t *)dst;
    for (int i = 0; i < n; i++) d[i] = val;
}

static void ccmp_xor(uint8_t *dst, const uint8_t *src, int n) {
    for (int i = 0; i < n; i++) dst[i] ^= src[i];
}

/* ── AES-CCM constants ─────────────────────────────────────────────── */

#define CCMP_MIC_LEN    8   /* M = 8 bytes */
#define CCMP_HDR_LEN    8   /* CCMP header (IV + ExtIV) */
#define CCMP_NONCE_LEN  13  /* nonce: priority(1) + addr2(6) + pn(6) */

/* ── Self-test: FIPS 197 AES + RFC 3610 CCM Vector #1 ────────────── */

static int ccmp_self_test_done = 0;

void ccmp_self_test(void) {
    if (ccmp_self_test_done) return;
    ccmp_self_test_done = 1;

    efi_print_ascii("CCMP self-test:\r\n");

    /* ── FIPS 197 Appendix B: AES-128 encrypt ─────────────────── */
    {
        static const uint8_t fkey[16] = {
            0x2B,0x7E,0x15,0x16,0x28,0xAE,0xD2,0xA6,
            0xAB,0xF7,0x15,0x88,0x09,0xCF,0x4F,0x3C
        };
        static const uint8_t fpt[16] = {
            0x32,0x43,0xF6,0xA8,0x88,0x5A,0x30,0x8D,
            0x31,0x31,0x98,0xA2,0xE0,0x37,0x07,0x34
        };
        static const uint8_t fct[16] = {
            0x39,0x25,0x84,0x1D,0x02,0xDC,0x09,0xFB,
            0xDC,0x11,0x85,0x97,0x19,0x6A,0x0B,0x32
        };

        iwn_aes_ctx a;
        iwn_aes128_init(&a, fkey);
        uint8_t out[16];
        iwn_aes128_encrypt(&a, fpt, out);

        uint8_t ok = 1;
        for (int i = 0; i < 16; i++)
            if (out[i] != fct[i]) ok = 0;

        efi_print_ascii("  AES-128: ");
        for (int i = 0; i < 4; i++) print_hex8(out[i]);
        efi_print_ascii(ok ? " PASS\r\n" : " FAIL\r\n");
    }

    /* ── RFC 3610 Packet Vector #1 (M=8, L=2) ────────────────── */
    {
        static const uint8_t key[16] = {
            0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,
            0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF
        };
        static const uint8_t nonce[13] = {
            0x00,0x00,0x00,0x03,0x02,0x01,0x00,
            0xA0,0xA1,0xA2,0xA3,0xA4,0xA5
        };
        static const uint8_t aad[8] = {
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07
        };
        int aad_len = 8;
        static const uint8_t exp_pt[23] = {
            0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
            0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
            0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E
        };
        /* Ciphertext(23) + MIC(8) from RFC */
        uint8_t ct[31] = {
            0x58,0x8C,0x97,0x9A,0x61,0xC6,0x63,0xD2,
            0xF0,0x66,0xD0,0xC2,0xC0,0xF9,0x89,0x80,
            0x6D,0x5F,0x6B,0x61,0xDA,0xC3,0x84,
            0x17,0xE8,0xD1,0x2C,0xFD,0xF9,0x26,0xE0
        };
        int ct_len = 23;
        uint8_t *mic = ct + ct_len;

        iwn_aes_ctx a;
        iwn_aes128_init(&a, key);

        /* Step 1: AES-CTR decrypt ciphertext (counters 1,2,...) */
        {
            uint8_t ctr[16], ks[16];
            ctr[0] = 0x01;
            ccmp_memcpy(ctr + 1, nonce, 13);
            int pos = 0;
            uint16_t c = 1;
            while (pos < ct_len) {
                ctr[14] = (uint8_t)(c >> 8);
                ctr[15] = (uint8_t)(c);
                iwn_aes128_encrypt(&a, ctr, ks);
                int bl = ct_len - pos;
                if (bl > 16) bl = 16;
                ccmp_xor(ct + pos, ks, bl);
                pos += bl;
                c++;
            }
        }

        /* Verify plaintext */
        uint8_t pt_ok = 1;
        for (int i = 0; i < ct_len; i++)
            if (ct[i] != exp_pt[i]) pt_ok = 0;

        efi_print_ascii("  CCM CTR: ");
        for (int i = 0; i < 8; i++) print_hex8(ct[i]);
        efi_print_ascii(pt_ok ? " PASS\r\n" : " FAIL\r\n");

        /* Step 2: CBC-MAC over B_0 + AAD + plaintext */
        uint8_t T[16];
        {
            uint8_t B[16];
            B[0] = 0x59;
            ccmp_memcpy(B + 1, nonce, 13);
            B[14] = (uint8_t)(ct_len >> 8);
            B[15] = (uint8_t)(ct_len);

            iwn_aes128_encrypt(&a, B, T);

            efi_print_ascii("    X0=");
            for (int i = 0; i < 16; i++) print_hex8(T[i]);
            efi_print_ascii("\r\n");

            /* AAD block: [len_hi][len_lo][aad...][zero-pad to 16] */
            uint8_t ab[32];
            ccmp_memset(ab, 0, 32);
            ab[0] = (uint8_t)(aad_len >> 8);
            ab[1] = (uint8_t)(aad_len);
            ccmp_memcpy(ab + 2, aad, aad_len);

            int at = 2 + aad_len;
            int ap = ((at + 15) / 16) * 16;

            for (int i = 0; i < ap; i += 16) {
                ccmp_xor(T, ab + i, 16);
                iwn_aes128_encrypt(&a, T, T);
            }

            efi_print_ascii("    Taad=");
            for (int i = 0; i < 16; i++) print_hex8(T[i]);
            efi_print_ascii("\r\n");

            /* Plaintext blocks (ct[] now holds plaintext) */
            int pos = 0;
            while (pos < ct_len) {
                int bl = ct_len - pos;
                if (bl > 16) bl = 16;
                ccmp_xor(T, ct + pos, bl);
                iwn_aes128_encrypt(&a, T, T);
                pos += 16;
            }

            efi_print_ascii("    Tpt=");
            for (int i = 0; i < 16; i++) print_hex8(T[i]);
            efi_print_ascii("\r\n");
        }

        /* Step 3: S_0 = AES(CTR counter 0) */
        uint8_t S0[16];
        {
            uint8_t c0[16];
            c0[0] = 0x01;
            ccmp_memcpy(c0 + 1, nonce, 13);
            c0[14] = 0x00;
            c0[15] = 0x00;
            iwn_aes128_encrypt(&a, c0, S0);
        }

        /* U = T XOR S0 (first 8 bytes) */
        uint8_t U[8];
        for (int i = 0; i < 8; i++)
            U[i] = T[i] ^ S0[i];

        /* Compare with received MIC */
        uint8_t mic_ok = 1;
        for (int i = 0; i < 8; i++)
            if (U[i] != mic[i]) mic_ok = 0;

        efi_print_ascii("  CCM MIC: comp=");
        for (int i = 0; i < 8; i++) print_hex8(U[i]);
        efi_print_ascii(" rx=");
        for (int i = 0; i < 8; i++) print_hex8(mic[i]);
        efi_print_ascii(mic_ok ? " PASS\r\n" : " FAIL\r\n");
    }

    /* ── Test 3: Full 802.11 CCMP frame through iwn_ccmp_decrypt ── */
    /* Build a complete 802.11 data frame, encrypt it with CCM,
     * then call iwn_ccmp_decrypt() to verify the full code path:
     * FC parsing, CCMP header, nonce/AAD construction, decrypt, MIC. */
    {
        static const uint8_t tkey[16] = {
            0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,
            0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA
        };
        static const uint8_t pt[10] = { /* LLC/SNAP IPv4 + "Hi" */
            0xAA,0xAA,0x03,0x00,0x00,0x00,0x08,0x00,
            0x48,0x69
        };
        int pt_len = 10;

        /* [MAC hdr 24] [CCMP hdr 8] [ciphertext 10] [MIC 8] = 50 */
        uint8_t frm[64];
        ccmp_memset(frm, 0, 64);

        /* MAC header: data, FromDS=1, Protected=1
         * NOTE: iwn_ccmp_decrypt() reconstructs QoS AAD (fc0=0x88,
         * aad_len=24) because the Intel 6000 firmware strips QoS.
         * The synthetic frame uses non-QoS fc (0x08) to match what
         * the firmware delivers, but the decrypt path will use
         * fc0=0x88 in the AAD. So we must also encrypt with QoS AAD. */
        frm[0] = 0x08;  /* fc0: type=2, subtype=0 (firmware-stripped) */
        frm[1] = 0x42;  /* fc1: FromDS=1, Protected=1 */
        /* Addr1 (DA): broadcast */
        frm[4]=0xFF; frm[5]=0xFF; frm[6]=0xFF;
        frm[7]=0xFF; frm[8]=0xFF; frm[9]=0xFF;
        /* Addr2 (BSSID) */
        frm[10]=0x00; frm[11]=0x11; frm[12]=0x22;
        frm[13]=0x33; frm[14]=0x44; frm[15]=0x55;
        /* Addr3 (SA) */
        frm[16]=0x00; frm[17]=0x66; frm[18]=0x77;
        frm[19]=0x88; frm[20]=0x99; frm[21]=0xBB;
        /* SC: seq=1, frag=0 */
        frm[22] = 0x10; frm[23] = 0x00;

        /* CCMP header at offset 24: PN=1 */
        frm[24] = 0x01;  /* PN0 */
        frm[25] = 0x00;  /* PN1 */
        frm[26] = 0x00;  /* reserved */
        frm[27] = 0x20;  /* ExtIV=1, KeyID=0 */
        frm[28] = 0x00;  /* PN2 */
        frm[29] = 0x00;  /* PN3 */
        frm[30] = 0x00;  /* PN4 */
        frm[31] = 0x00;  /* PN5 */

        /* Encrypt: build nonce/AAD matching what iwn_ccmp_decrypt will build */
        iwn_aes_ctx a;
        iwn_aes128_init(&a, tkey);

        uint8_t tn[13]; /* nonce */
        tn[0] = 0;  /* non-QoS priority */
        ccmp_memcpy(tn + 1, frm + 10, 6); /* Addr2 */
        tn[7]=0; tn[8]=0; tn[9]=0; tn[10]=0; tn[11]=0; tn[12]=0x01; /* PN BE */

        /* AAD: masked FC + Addr1 + Addr2 + Addr3 + masked SC + QoS
         * Use QoS AAD (fc0=0x88, 24 bytes) to match iwn_ccmp_decrypt() */
        uint8_t ta[24];
        ta[0] = 0x88;              /* fc0 = QoS data (reconstructed) */
        ta[1] = frm[1] & 0x07;    /* fc1 masked = 0x02 */
        ccmp_memcpy(ta + 2, frm + 4, 6);
        ccmp_memcpy(ta + 8, frm + 10, 6);
        ccmp_memcpy(ta + 14, frm + 16, 6);
        ta[20] = frm[22] & 0x0f;
        ta[21] = 0x00;
        ta[22] = 0x00;             /* QoS TC: TID=0 */
        ta[23] = 0x00;

        /* CBC-MAC over B_0 + AAD + plaintext → T */
        uint8_t T[16];
        {
            uint8_t B[16];
            B[0] = 0x59;
            ccmp_memcpy(B + 1, tn, 13);
            B[14] = 0; B[15] = (uint8_t)pt_len;
            iwn_aes128_encrypt(&a, B, T);

            uint8_t ab[32];
            ccmp_memset(ab, 0, 32);
            ab[0] = 0; ab[1] = 24; /* aad_len = 24 (QoS) */
            ccmp_memcpy(ab + 2, ta, 24);
            /* 2+24=26, padded to 32 */
            ccmp_xor(T, ab, 16);
            iwn_aes128_encrypt(&a, T, T);
            ccmp_xor(T, ab + 16, 16);
            iwn_aes128_encrypt(&a, T, T);

            /* Plaintext (10 bytes, one partial block) */
            ccmp_xor(T, pt, pt_len);
            iwn_aes128_encrypt(&a, T, T);
        }

        /* S_0 → encrypted MIC */
        uint8_t S0[16];
        {
            uint8_t c0[16];
            c0[0] = 0x01;
            ccmp_memcpy(c0 + 1, tn, 13);
            c0[14] = 0; c0[15] = 0;
            iwn_aes128_encrypt(&a, c0, S0);
        }
        uint8_t emic[8];
        for (int i = 0; i < 8; i++) emic[i] = T[i] ^ S0[i];

        /* CTR encrypt plaintext → ciphertext */
        uint8_t ctbuf[16];
        ccmp_memcpy(ctbuf, pt, pt_len);
        {
            uint8_t ctr[16], ks[16];
            ctr[0] = 0x01;
            ccmp_memcpy(ctr + 1, tn, 13);
            ctr[14] = 0; ctr[15] = 1;
            iwn_aes128_encrypt(&a, ctr, ks);
            ccmp_xor(ctbuf, ks, pt_len);
        }

        /* Assemble: [hdr 24][ccmp 8][ciphertext 10][MIC 8] */
        ccmp_memcpy(frm + 32, ctbuf, pt_len);
        ccmp_memcpy(frm + 32 + pt_len, emic, 8);
        int flen = 24 + 8 + pt_len + 8; /* 50 */

        /* Call iwn_ccmp_decrypt — it will print aad= and CCMP diagnostics */
        int rc = iwn_ccmp_decrypt(frm, flen, tkey);

        /* Verify plaintext recovered */
        uint8_t pt_ok = 1;
        if (rc != 0) pt_ok = 0;
        for (int i = 0; i < pt_len && pt_ok; i++)
            if (frm[32 + i] != pt[i]) pt_ok = 0;

        efi_print_ascii("  802.11 frame: ");
        efi_print_ascii(pt_ok ? "PASS\r\n" : "FAIL\r\n");
    }
}

/*
 * iwn_ccmp_decrypt — Decrypt a CCMP-encrypted 802.11 data frame in-place.
 *
 * frame:     points to the 802.11 header (FC at frame[0])
 * frame_len: total length including 802.11 hdr + CCMP hdr + ciphertext + MIC
 * key:       16-byte temporal key (PTK TK or GTK)
 *
 * On success: decrypts ciphertext in-place, returns 0.
 * On MIC failure: returns -1 (frame should be dropped).
 *
 * Frame layout on input:
 *   [802.11 hdr (24/26B)] [CCMP hdr (8B)] [ciphertext (N)] [MIC (8B)]
 *
 * After successful decrypt:
 *   [802.11 hdr (24/26B)] [CCMP hdr (8B)] [plaintext (N)] [MIC (8B)]
 *   (caller still needs to skip CCMP hdr and strip MIC — existing code does this)
 */
int iwn_ccmp_decrypt(uint8_t *frame, int frame_len,
                     const uint8_t *key) {
    ccmp_self_test();

    /* Determine 802.11 header length */
    uint8_t fc0 = frame[0];
    uint8_t subtype = (fc0 >> 4) & 0x0f;
    int hdr_len = 24;
    if (subtype == 8)  /* QoS data */
        hdr_len = 26;

    /* Sanity check: need at least header + CCMP hdr + MIC */
    if (frame_len < hdr_len + CCMP_HDR_LEN + CCMP_MIC_LEN)
        return -1;

    /* ── 1. Parse CCMP header — extract 48-bit PN ──────────────────── */
    uint8_t *ccmp = frame + hdr_len;
    uint8_t pn[6];
    pn[0] = ccmp[0];  /* PN0 (least significant) */
    pn[1] = ccmp[1];  /* PN1 */
    pn[2] = ccmp[4];  /* PN2 */
    pn[3] = ccmp[5];  /* PN3 */
    pn[4] = ccmp[6];  /* PN4 */
    pn[5] = ccmp[7];  /* PN5 (most significant) */

    /* Ciphertext starts after CCMP header, ends before MIC */
    uint8_t *ctext = frame + hdr_len + CCMP_HDR_LEN;
    int ctext_len = frame_len - hdr_len - CCMP_HDR_LEN - CCMP_MIC_LEN;
    if (ctext_len < 0)
        return -1;

    /* MIC is the last 8 bytes */
    uint8_t *rx_mic = ctext + ctext_len;

    /* ── 2. Build 13-byte nonce ────────────────────────────────────── */
    /* [priority(1)] [addr2(6)] [pn5 pn4 pn3 pn2 pn1 pn0] */
    uint8_t nonce[CCMP_NONCE_LEN];
    if (subtype == 8)
        nonce[0] = frame[24] & 0x0f;  /* QoS TID */
    else
        nonce[0] = 0;                  /* non-QoS priority */
    ccmp_memcpy(nonce + 1, frame + 10, 6);  /* Addr2 (transmitter) */
    nonce[7]  = pn[5];
    nonce[8]  = pn[4];
    nonce[9]  = pn[3];
    nonce[10] = pn[2];
    nonce[11] = pn[1];
    nonce[12] = pn[0];

    /* ── 3. Build AAD from 802.11 header ──────────────────────────── */
    /* AAD = masked FC + Addr1 + Addr2 + Addr3 + masked SC [+ QoS]
     *
     * IMPORTANT: The Intel 6000 firmware strips the 2-byte QoS Control
     * field from received frames, converting QoS data (subtype 8,
     * fc0=0x88) to plain data (subtype 0, fc0=0x08). But the AP
     * encrypted with QoS AAD. We must reconstruct the original QoS
     * AAD to match the AP's MIC computation. */
    uint8_t aad[30];
    int aad_len;

    /* fc[1] masked: clear Retry(3), PwrMgmt(4), MoreData(5), Protected(6), Order(7) */
    uint8_t fc1_masked = frame[1] & 0x07;  /* keep bits 0-2 (ToDS, FromDS, MoreFrag) */

    /* Reconstruct original QoS fc0: set subtype to QoS data (8) */
    uint8_t fc0_qos = 0x88;  /* type=2 (data), subtype=8 (QoS) */
    aad[0] = fc0_qos;
    aad[1] = fc1_masked;
    ccmp_memcpy(aad + 2, frame + 4, 6);   /* Addr1 */
    ccmp_memcpy(aad + 8, frame + 10, 6);  /* Addr2 */
    ccmp_memcpy(aad + 14, frame + 16, 6); /* Addr3 */
    /* SC masked: clear sequence number (bits 4-15), keep fragment (bits 0-3) */
    aad[20] = frame[22] & 0x0f;
    aad[21] = 0x00;
    /* QoS TC: TID=0 (best effort), rest cleared */
    aad[22] = 0x00;
    aad[23] = 0x00;
    aad_len = 24;

    /* Print AAD for diagnostics */
    efi_print_ascii("    aad=");
    for (int i = 0; i < aad_len; i++)
        print_hex8(aad[i]);
    efi_print_ascii("\r\n");

    /* ── 4. Initialize AES context ─────────────────────────────────── */
    iwn_aes_ctx aes;
    iwn_aes128_init(&aes, key);

    /* AES sanity: encrypt all-zeros and print first 4 bytes */
    {
        uint8_t z[16], e[16];
        ccmp_memset(z, 0, 16);
        iwn_aes128_encrypt(&aes, z, e);
        efi_print_ascii("    aes0=");
        print_hex8(e[0]); print_hex8(e[1]);
        print_hex8(e[2]); print_hex8(e[3]);
        efi_print_ascii("\r\n");
    }

    /* ── PROBE: brute-force MIC position on first frame ────────────── */
    static int probe_done = 0;
    static int flen_adj = 0;

    if (!probe_done) {
        probe_done = 1;

        /* Print raw frame tail (last 16 bytes + 4 beyond) before CTR */
        efi_print_ascii("  PROBE flen=");
        print_dec((uint32_t)frame_len);
        efi_print_ascii(" tail=");
        {
            int s = frame_len - 16;
            if (s < 0) s = 0;
            for (int i = s; i < frame_len + 4; i++)
                print_hex8(frame[i]);
        }
        efi_print_ascii("\r\n");

        /* 4 adjustments to try: 0, 2, 4, 6 bytes off ctext_len */
        int adj[4] = {0, 2, 4, 6};

        /* Save MIC at 4 candidate positions BEFORE CTR corrupts them */
        uint8_t smic[4][8];
        for (int t = 0; t < 4; t++) {
            int tc = ctext_len - adj[t];
            uint8_t *mp = ctext + tc;
            if (tc >= 0 && mp + 8 <= frame + frame_len + 4)
                ccmp_memcpy(smic[t], mp, 8);
            else
                ccmp_memset(smic[t], 0xFF, 8);
        }

        /* CTR decrypt with MAX ctext_len (adj=0) */
        {
            uint8_t cb[16], ks[16];
            cb[0] = 0x01;
            ccmp_memcpy(cb + 1, nonce, CCMP_NONCE_LEN);
            int pos = 0;
            uint16_t c = 1;
            while (pos < ctext_len) {
                cb[14] = (uint8_t)(c >> 8);
                cb[15] = (uint8_t)(c);
                iwn_aes128_encrypt(&aes, cb, ks);
                int bl = ctext_len - pos;
                if (bl > 16) bl = 16;
                ccmp_xor(ctext + pos, ks, bl);
                pos += bl;
                c++;
            }
        }

        /* S0 for MIC XOR */
        uint8_t S0[16];
        {
            uint8_t c0[16];
            c0[0] = 0x01;
            ccmp_memcpy(c0 + 1, nonce, CCMP_NONCE_LEN);
            c0[14] = 0; c0[15] = 0;
            iwn_aes128_encrypt(&aes, c0, S0);
        }

        efi_print_ascii("  dec=");
        for (int i = 0; i < 8 && i < ctext_len; i++)
            print_hex8(ctext[i]);
        efi_print_ascii("\r\n");

        /* Build non-QoS AAD for comparison */
        uint8_t nq_aad[22];
        nq_aad[0] = fc0;
        nq_aad[1] = frame[1] & 0x07;
        ccmp_memcpy(nq_aad + 2, frame + 4, 6);
        ccmp_memcpy(nq_aad + 8, frame + 10, 6);
        ccmp_memcpy(nq_aad + 14, frame + 16, 6);
        nq_aad[20] = frame[22] & 0x0f;
        nq_aad[21] = 0x00;

        /* Try 4 adjustments × 2 AAD types = 8 combinations */
        for (int q = 1; q >= 0; q--) {
            uint8_t *try_aad = q ? aad : nq_aad;
            int try_al = q ? aad_len : 22;

            for (int t = 0; t < 4; t++) {
                int tc = ctext_len - adj[t];
                if (tc < 0) continue;

                /* CBC-MAC */
                uint8_t T[16], B[16];
                B[0] = 0x59;
                ccmp_memcpy(B + 1, nonce, CCMP_NONCE_LEN);
                B[14] = (uint8_t)(tc >> 8);
                B[15] = (uint8_t)(tc);
                iwn_aes128_encrypt(&aes, B, T);

                uint8_t ab[32];
                ccmp_memset(ab, 0, 32);
                ab[0] = (uint8_t)(try_al >> 8);
                ab[1] = (uint8_t)(try_al);
                ccmp_memcpy(ab + 2, try_aad, try_al);
                int ap = ((2 + try_al + 15) / 16) * 16;
                for (int i = 0; i < ap; i += 16) {
                    ccmp_xor(T, ab + i, 16);
                    iwn_aes128_encrypt(&aes, T, T);
                }

                int pos = 0;
                while (pos < tc) {
                    int bl = tc - pos;
                    if (bl > 16) bl = 16;
                    ccmp_xor(T, ctext + pos, bl);
                    iwn_aes128_encrypt(&aes, T, T);
                    pos += 16;
                }

                uint8_t U[8];
                for (int i = 0; i < 8; i++)
                    U[i] = T[i] ^ S0[i];

                uint8_t m = 1;
                for (int i = 0; i < 8; i++)
                    if (U[i] != smic[t][i]) m = 0;

                efi_print_ascii("  ");
                efi_print_ascii(q ? "Q" : "N");
                efi_print_ascii("-");
                print_dec((uint32_t)adj[t]);
                efi_print_ascii(" ct=");
                print_dec((uint32_t)tc);
                efi_print_ascii(": ");
                for (int i = 0; i < 8; i++) print_hex8(U[i]);
                if (m) {
                    efi_print_ascii(" MATCH!\r\n");
                    flen_adj = adj[t];
                } else {
                    efi_print_ascii(" fail\r\n");
                }
            }
        }

        if (flen_adj > 0) {
            efi_print_ascii("  WINNER: adj=");
            print_dec((uint32_t)flen_adj);
            efi_print_ascii("\r\n");
        } else {
            efi_print_ascii("  NO MATCH in 8 combinations\r\n");
        }

        return -1;  /* sacrifice probe frame */
    }

    /* Apply discovered adjustment for subsequent frames */
    ctext_len -= flen_adj;
    rx_mic = ctext + ctext_len;
    if (ctext_len < 0)
        return -1;

    /* ── 5. AES-CTR decrypt ────────────────────────────────────────── */
    /* Counter block A_i: [flags(1)] [nonce(13)] [counter(2)]
     * flags = 0x01 (L=2, no Adata flag for CTR blocks)
     * counter = 1, 2, 3, ... for data; 0 for MIC encryption */
    {
        uint8_t ctr_blk[16];
        uint8_t keystream[16];

        ctr_blk[0] = 0x01;  /* flags: L-1 = 1 */
        ccmp_memcpy(ctr_blk + 1, nonce, CCMP_NONCE_LEN);

        int pos = 0;
        uint16_t counter = 1;
        while (pos < ctext_len) {
            /* Set counter (big-endian) */
            ctr_blk[14] = (uint8_t)(counter >> 8);
            ctr_blk[15] = (uint8_t)(counter);

            iwn_aes128_encrypt(&aes, ctr_blk, keystream);

            int block_len = ctext_len - pos;
            if (block_len > 16) block_len = 16;
            ccmp_xor(ctext + pos, keystream, block_len);

            pos += block_len;
            counter++;
        }
    }

    /* Now ctext contains plaintext */

    /* ── 6. CBC-MAC verify ─────────────────────────────────────────── */
    /* B_0 = [flags(1)] [nonce(13)] [len(2)]
     * flags = 0x59 (Adata=1, M=(8-2)/2=3, L-1=1 → 0x40|0x18|0x01 = 0x59)
     *
     * CBC-MAC chain:
     *   X_0 = AES(B_0)
     *   X_1 = AES(X_0 XOR B_1)  where B_1 = aad_len_encoded || AAD || padding
     *   ...then plaintext blocks
     */
    uint8_t T[16];  /* CBC-MAC accumulator */
    {
        uint8_t B[16];

        /* B_0 */
        B[0] = 0x59;  /* flags */
        ccmp_memcpy(B + 1, nonce, CCMP_NONCE_LEN);
        B[14] = (uint8_t)(ctext_len >> 8);
        B[15] = (uint8_t)(ctext_len);

        iwn_aes128_encrypt(&aes, B, T);

        /* AAD: encode as [len_hi] [len_lo] [aad_bytes...] padded to 16-byte blocks */
        uint8_t aad_block[32];  /* max 2 blocks for AAD */
        ccmp_memset(aad_block, 0, sizeof(aad_block));
        aad_block[0] = (uint8_t)(aad_len >> 8);
        aad_block[1] = (uint8_t)(aad_len);
        ccmp_memcpy(aad_block + 2, aad, aad_len);

        int aad_total = 2 + aad_len;  /* length-encoded AAD */
        /* Pad to multiple of 16 */
        int aad_padded = ((aad_total + 15) / 16) * 16;

        for (int i = 0; i < aad_padded; i += 16) {
            ccmp_xor(T, aad_block + i, 16);
            iwn_aes128_encrypt(&aes, T, T);
        }

        /* Plaintext blocks */
        int pos = 0;
        while (pos < ctext_len) {
            int block_len = ctext_len - pos;
            if (block_len > 16) block_len = 16;

            /* XOR plaintext into T (partial last block: only block_len bytes) */
            ccmp_xor(T, ctext + pos, block_len);
            iwn_aes128_encrypt(&aes, T, T);

            pos += 16;
        }
    }

    /* ── 7. Encrypt T with counter 0 to get U, compare with received MIC ── */
    {
        uint8_t ctr0[16];
        uint8_t S0[16];

        ctr0[0] = 0x01;  /* flags */
        ccmp_memcpy(ctr0 + 1, nonce, CCMP_NONCE_LEN);
        ctr0[14] = 0x00;
        ctr0[15] = 0x00;

        iwn_aes128_encrypt(&aes, ctr0, S0);

        /* U = T XOR S0 (first 8 bytes) */
        uint8_t U[CCMP_MIC_LEN];
        for (int i = 0; i < CCMP_MIC_LEN; i++)
            U[i] = T[i] ^ S0[i];

        /* Constant-time compare with received MIC */
        uint8_t diff = 0;
        for (int i = 0; i < CCMP_MIC_LEN; i++)
            diff |= U[i] ^ rx_mic[i];

        /* Diagnostic output */
        efi_print_ascii("  CCMP: hdr=");
        print_dec((uint32_t)hdr_len);
        efi_print_ascii(" ct=");
        print_dec((uint32_t)ctext_len);
        efi_print_ascii(" key=");
        print_hex8(key[0]); print_hex8(key[1]);
        print_hex8(key[2]); print_hex8(key[3]);
        efi_print_ascii("\r\n");
        efi_print_ascii("    nonce=");
        for (int i = 0; i < CCMP_NONCE_LEN; i++)
            print_hex8(nonce[i]);
        efi_print_ascii("\r\n");
        /* Always print first 8 bytes of decrypted data (after CTR) */
        efi_print_ascii("    dec=");
        {
            int plen = ctext_len < 8 ? ctext_len : 8;
            for (int i = 0; i < plen; i++)
                print_hex8(ctext[i]);
        }
        efi_print_ascii("\r\n");
        efi_print_ascii("    comp=");
        for (int i = 0; i < CCMP_MIC_LEN; i++)
            print_hex8(U[i]);
        efi_print_ascii(" rx=");
        for (int i = 0; i < CCMP_MIC_LEN; i++)
            print_hex8(rx_mic[i]);
        efi_print_ascii(diff ? " FAIL" : " OK");
        efi_print_ascii("\r\n");

        if (diff != 0)
            return -1;  /* MIC failure */
    }

    return 0;  /* success */
}
