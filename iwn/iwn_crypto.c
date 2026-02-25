/*
 * iwn_crypto.c — WPA2-PSK crypto primitives
 *
 * Self-contained: no libc, no UEFI. Own memcpy/memset/memcmp.
 * Compiles with both host cc and x86_64-elf-gcc cross-compiler.
 *
 * Implements:
 *   1. SHA-1        (FIPS 180-4)
 *   2. HMAC-SHA1    (RFC 2104)
 *   3. PBKDF2-SHA1  (RFC 2898)
 *   4. AES-128      (FIPS 197)
 *   5. AES Unwrap   (RFC 3394)
 *   6. PRF-384      (IEEE 802.11i)
 */

#include "iwn_crypto.h"

/* ═══════════════════════════════════════════════════════════════════════
 * Static helpers — no libc
 * ═══════════════════════════════════════════════════════════════════════ */

static void crypto_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void crypto_memset(void *dst, uint8_t val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = val;
}

__attribute__((unused))
static int crypto_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * Endian helpers — SHA-1 uses big-endian
 * ═══════════════════════════════════════════════════════════════════════ */

static uint32_t be32_load(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void be32_store(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static void be64_store(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)(v);
}

/* ═══════════════════════════════════════════════════════════════════════
 * SHA-1 (FIPS 180-4)
 * ═══════════════════════════════════════════════════════════════════════ */

#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t W[80];
    uint32_t a, b, c, d, e;

    /* Load 16 words big-endian */
    for (int i = 0; i < 16; i++)
        W[i] = be32_load(block + i * 4);

    /* Expand to 80 words */
    for (int i = 16; i < 80; i++)
        W[i] = ROTL(W[i-3] ^ W[i-8] ^ W[i-14] ^ W[i-16], 1);

    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        uint32_t temp = ROTL(a, 5) + f + e + k + W[i];
        e = d;
        d = c;
        c = ROTL(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void iwn_sha1_init(iwn_sha1_ctx *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->buf_len = 0;
    ctx->total_len = 0;
}

void iwn_sha1_update(iwn_sha1_ctx *ctx, const uint8_t *data, size_t len) {
    ctx->total_len += len;

    /* Fill buffer first */
    if (ctx->buf_len > 0) {
        uint32_t fill = 64 - ctx->buf_len;
        if (len < fill) {
            crypto_memcpy(ctx->buf + ctx->buf_len, data, len);
            ctx->buf_len += (uint32_t)len;
            return;
        }
        crypto_memcpy(ctx->buf + ctx->buf_len, data, fill);
        sha1_transform(ctx->state, ctx->buf);
        data += fill;
        len -= fill;
        ctx->buf_len = 0;
    }

    /* Process full blocks */
    while (len >= 64) {
        sha1_transform(ctx->state, data);
        data += 64;
        len -= 64;
    }

    /* Buffer remainder */
    if (len > 0) {
        crypto_memcpy(ctx->buf, data, len);
        ctx->buf_len = (uint32_t)len;
    }
}

void iwn_sha1_final(iwn_sha1_ctx *ctx, uint8_t digest[20]) {
    uint64_t total_bits = ctx->total_len * 8;

    /* Append 0x80 */
    ctx->buf[ctx->buf_len++] = 0x80;

    /* If not enough room for length (need 8 bytes), pad and flush */
    if (ctx->buf_len > 56) {
        crypto_memset(ctx->buf + ctx->buf_len, 0, 64 - ctx->buf_len);
        sha1_transform(ctx->state, ctx->buf);
        ctx->buf_len = 0;
    }

    /* Pad to 56 bytes */
    crypto_memset(ctx->buf + ctx->buf_len, 0, 56 - ctx->buf_len);

    /* Append total length in bits as big-endian 64-bit */
    be64_store(ctx->buf + 56, total_bits);
    sha1_transform(ctx->state, ctx->buf);

    /* Output digest */
    for (int i = 0; i < 5; i++)
        be32_store(digest + i * 4, ctx->state[i]);
}

void iwn_sha1(const uint8_t *data, size_t len, uint8_t digest[20]) {
    iwn_sha1_ctx ctx;
    iwn_sha1_init(&ctx);
    iwn_sha1_update(&ctx, data, len);
    iwn_sha1_final(&ctx, digest);
}

/* ═══════════════════════════════════════════════════════════════════════
 * HMAC-SHA1 (RFC 2104)
 * ═══════════════════════════════════════════════════════════════════════ */

#define HMAC_BLOCK_SIZE 64

void iwn_hmac_sha1(const uint8_t *key, size_t key_len,
                   const uint8_t *data, size_t data_len,
                   uint8_t mac[20]) {
    uint8_t k_pad[HMAC_BLOCK_SIZE];
    uint8_t tk[20]; /* hashed key if key > block size */
    iwn_sha1_ctx ctx;

    /* If key > block size, hash it first */
    if (key_len > HMAC_BLOCK_SIZE) {
        iwn_sha1(key, key_len, tk);
        key = tk;
        key_len = 20;
    }

    /* Prepare ipad key (key XOR 0x36, zero-padded to block size) */
    crypto_memset(k_pad, 0x36, HMAC_BLOCK_SIZE);
    for (size_t i = 0; i < key_len; i++)
        k_pad[i] ^= key[i];

    /* Inner hash: SHA1(K ^ ipad || data) */
    iwn_sha1_init(&ctx);
    iwn_sha1_update(&ctx, k_pad, HMAC_BLOCK_SIZE);
    iwn_sha1_update(&ctx, data, data_len);
    uint8_t inner[20];
    iwn_sha1_final(&ctx, inner);

    /* Prepare opad key (key XOR 0x5C, zero-padded to block size) */
    crypto_memset(k_pad, 0x5C, HMAC_BLOCK_SIZE);
    for (size_t i = 0; i < key_len; i++)
        k_pad[i] ^= key[i];

    /* Outer hash: SHA1(K ^ opad || inner) */
    iwn_sha1_init(&ctx);
    iwn_sha1_update(&ctx, k_pad, HMAC_BLOCK_SIZE);
    iwn_sha1_update(&ctx, inner, 20);
    iwn_sha1_final(&ctx, mac);
}

/* ═══════════════════════════════════════════════════════════════════════
 * PBKDF2-SHA1 (RFC 2898)
 * ═══════════════════════════════════════════════════════════════════════ */

void iwn_pbkdf2_sha1(const uint8_t *password, size_t password_len,
                     const uint8_t *salt, size_t salt_len,
                     uint32_t iterations,
                     uint8_t *output, size_t output_len) {
    uint8_t U[20];    /* Current HMAC output */
    uint8_t T[20];    /* XOR accumulator for this block */
    uint32_t block = 1;
    size_t offset = 0;

    while (offset < output_len) {
        /* U1 = HMAC(password, salt || INT_BE(block)) */
        {
            /* Build salt || INT_BE(block) in a temp buffer.
             * salt_len is small in practice (SSID, max 32 bytes) */
            uint8_t salt_block[128]; /* enough for any SSID + 4 */
            crypto_memcpy(salt_block, salt, salt_len);
            be32_store(salt_block + salt_len, block);
            iwn_hmac_sha1(password, password_len,
                          salt_block, salt_len + 4, U);
        }

        /* T = U1 */
        crypto_memcpy(T, U, 20);

        /* U2 .. U_iterations */
        for (uint32_t j = 1; j < iterations; j++) {
            iwn_hmac_sha1(password, password_len, U, 20, U);
            for (int k = 0; k < 20; k++)
                T[k] ^= U[k];
        }

        /* Copy T to output (may be partial for last block) */
        size_t copy_len = output_len - offset;
        if (copy_len > 20) copy_len = 20;
        crypto_memcpy(output + offset, T, copy_len);

        offset += copy_len;
        block++;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * AES-128 (FIPS 197)
 * ═══════════════════════════════════════════════════════════════════════ */

/* Forward S-box */
static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

/* Inverse S-box */
static const uint8_t aes_inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

/* Round constants */
static const uint8_t aes_rcon[10] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

/* ── Key schedule ──────────────────────────────────────────────────── */

static uint32_t sub_word(uint32_t w) {
    return ((uint32_t)aes_sbox[(w >> 24) & 0xFF] << 24) |
           ((uint32_t)aes_sbox[(w >> 16) & 0xFF] << 16) |
           ((uint32_t)aes_sbox[(w >>  8) & 0xFF] <<  8) |
           ((uint32_t)aes_sbox[ w        & 0xFF]);
}

static uint32_t rot_word(uint32_t w) {
    return (w << 8) | (w >> 24);
}

void iwn_aes128_init(iwn_aes_ctx *ctx, const uint8_t key[16]) {
    /* Load key as 4 big-endian words */
    for (int i = 0; i < 4; i++)
        ctx->rk[i] = be32_load(key + i * 4);

    /* Expand to 44 words */
    for (int i = 4; i < 44; i++) {
        uint32_t temp = ctx->rk[i - 1];
        if (i % 4 == 0)
            temp = sub_word(rot_word(temp)) ^ ((uint32_t)aes_rcon[i / 4 - 1] << 24);
        ctx->rk[i] = ctx->rk[i - 4] ^ temp;
    }
}

/* ── Encrypt ───────────────────────────────────────────────────────── */

static uint8_t xtime(uint8_t a) {
    return (uint8_t)((a << 1) ^ (((a >> 7) & 1) * 0x1b));
}

static void aes_sub_bytes(uint8_t s[16]) {
    for (int i = 0; i < 16; i++)
        s[i] = aes_sbox[s[i]];
}

static void aes_shift_rows(uint8_t s[16]) {
    uint8_t t;
    /* Row 1: shift left 1 (column-major: indices 1,5,9,13) */
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    /* Row 2: shift left 2 (indices 2,6,10,14) */
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    /* Row 3: shift left 3 (indices 3,7,11,15) */
    t = s[3]; s[3] = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = t;
}

static void aes_mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        int i = c * 4;
        uint8_t a0 = s[i], a1 = s[i+1], a2 = s[i+2], a3 = s[i+3];
        uint8_t t = a0 ^ a1 ^ a2 ^ a3;
        s[i]   = a0 ^ xtime(a0 ^ a1) ^ t;
        s[i+1] = a1 ^ xtime(a1 ^ a2) ^ t;
        s[i+2] = a2 ^ xtime(a2 ^ a3) ^ t;
        s[i+3] = a3 ^ xtime(a3 ^ a0) ^ t;
    }
}

static void aes_add_round_key(uint8_t s[16], const uint32_t *rk) {
    for (int c = 0; c < 4; c++) {
        s[c*4]   ^= (uint8_t)(rk[c] >> 24);
        s[c*4+1] ^= (uint8_t)(rk[c] >> 16);
        s[c*4+2] ^= (uint8_t)(rk[c] >> 8);
        s[c*4+3] ^= (uint8_t)(rk[c]);
    }
}

void iwn_aes128_encrypt(const iwn_aes_ctx *ctx,
                        const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];

    /* Load state column-major: state[r + 4*c] = in[r + 4*c] */
    crypto_memcpy(s, in, 16);

    /* Initial round key */
    aes_add_round_key(s, ctx->rk);

    /* Rounds 1-9 */
    for (int r = 1; r <= 9; r++) {
        aes_sub_bytes(s);
        aes_shift_rows(s);
        aes_mix_columns(s);
        aes_add_round_key(s, ctx->rk + r * 4);
    }

    /* Final round (no MixColumns) */
    aes_sub_bytes(s);
    aes_shift_rows(s);
    aes_add_round_key(s, ctx->rk + 40);

    crypto_memcpy(out, s, 16);
}

/* ── Decrypt ───────────────────────────────────────────────────────── */

static void aes_inv_sub_bytes(uint8_t s[16]) {
    for (int i = 0; i < 16; i++)
        s[i] = aes_inv_sbox[s[i]];
}

static void aes_inv_shift_rows(uint8_t s[16]) {
    uint8_t t;
    /* Row 1: shift right 1 */
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    /* Row 2: shift right 2 */
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    /* Row 3: shift right 3 */
    t = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = s[3]; s[3] = t;
}

static void aes_inv_mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        int i = c * 4;
        uint8_t a0 = s[i], a1 = s[i+1], a2 = s[i+2], a3 = s[i+3];

        /* Multiply by inverse matrix {0e,0b,0d,09} */
        uint8_t x2_0 = xtime(a0), x2_1 = xtime(a1), x2_2 = xtime(a2), x2_3 = xtime(a3);
        uint8_t x4_0 = xtime(x2_0), x4_1 = xtime(x2_1), x4_2 = xtime(x2_2), x4_3 = xtime(x2_3);
        uint8_t x8_0 = xtime(x4_0), x8_1 = xtime(x4_1), x8_2 = xtime(x4_2), x8_3 = xtime(x4_3);

        /* 0x0e = x8 ^ x4 ^ x2 */
        /* 0x0b = x8 ^ x2 ^ x1 */
        /* 0x0d = x8 ^ x4 ^ x1 */
        /* 0x09 = x8 ^ x1       */
        s[i]   = (x8_0 ^ x4_0 ^ x2_0) ^ (x8_1 ^ x2_1 ^ a1) ^ (x8_2 ^ x4_2 ^ a2) ^ (x8_3 ^ a3);
        s[i+1] = (x8_0 ^ a0) ^ (x8_1 ^ x4_1 ^ x2_1) ^ (x8_2 ^ x2_2 ^ a2) ^ (x8_3 ^ x4_3 ^ a3);
        s[i+2] = (x8_0 ^ x4_0 ^ a0) ^ (x8_1 ^ a1) ^ (x8_2 ^ x4_2 ^ x2_2) ^ (x8_3 ^ x2_3 ^ a3);
        s[i+3] = (x8_0 ^ x2_0 ^ a0) ^ (x8_1 ^ x4_1 ^ a1) ^ (x8_2 ^ a2) ^ (x8_3 ^ x4_3 ^ x2_3);
    }
}

void iwn_aes128_decrypt(const iwn_aes_ctx *ctx,
                        const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    crypto_memcpy(s, in, 16);

    /* Initial round key (round 10) */
    aes_add_round_key(s, ctx->rk + 40);

    /* Rounds 9-1 */
    for (int r = 9; r >= 1; r--) {
        aes_inv_shift_rows(s);
        aes_inv_sub_bytes(s);
        aes_add_round_key(s, ctx->rk + r * 4);
        aes_inv_mix_columns(s);
    }

    /* Final round (no InvMixColumns) */
    aes_inv_shift_rows(s);
    aes_inv_sub_bytes(s);
    aes_add_round_key(s, ctx->rk);

    crypto_memcpy(out, s, 16);
}

/* ═══════════════════════════════════════════════════════════════════════
 * AES Key Unwrap (RFC 3394 Section 2.2.2)
 * ═══════════════════════════════════════════════════════════════════════ */

int iwn_aes_unwrap(const uint8_t kek[16],
                   const uint8_t *cipher, size_t cipher_len,
                   uint8_t *plain) {
    static const uint8_t default_iv[8] = {
        0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6
    };

    if (cipher_len < 16 || cipher_len % 8 != 0)
        return -1;

    size_t n = (cipher_len / 8) - 1; /* number of 64-bit data blocks */

    iwn_aes_ctx aes;
    iwn_aes128_init(&aes, kek);

    /* A = C[0], R[i] = C[i] for i = 1..n */
    uint8_t A[8];
    crypto_memcpy(A, cipher, 8);
    crypto_memcpy(plain, cipher + 8, n * 8);

    /* Unwrap: for j = 5 down to 0, for i = n down to 1 */
    for (int j = 5; j >= 0; j--) {
        for (size_t i = n; i >= 1; i--) {
            /* Compute counter = n*j + i */
            uint64_t t = (uint64_t)n * (uint64_t)j + (uint64_t)i;

            /* XOR counter into A (big-endian) */
            uint8_t t_bytes[8];
            be64_store(t_bytes, t);
            for (int k = 0; k < 8; k++)
                A[k] ^= t_bytes[k];

            /* B = AES_decrypt(KEK, A || R[i]) */
            uint8_t block[16];
            crypto_memcpy(block, A, 8);
            crypto_memcpy(block + 8, plain + (i - 1) * 8, 8);

            uint8_t dec[16];
            iwn_aes128_decrypt(&aes, block, dec);

            crypto_memcpy(A, dec, 8);
            crypto_memcpy(plain + (i - 1) * 8, dec + 8, 8);
        }
    }

    /* Check integrity: A must equal default IV */
    if (crypto_memcmp(A, default_iv, 8) != 0)
        return -1;

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * PRF-384 (IEEE 802.11i Section 8.5.1.1)
 * ═══════════════════════════════════════════════════════════════════════ */

void iwn_prf384(const uint8_t key[32],
                const uint8_t *label, size_t label_len,
                const uint8_t *data, size_t data_len,
                uint8_t output[48]) {
    /*
     * PRF-X(K, A, B) = HMAC-SHA1(K, A || 0x00 || B || i)
     * where i = 0, 1, 2, ... and we concatenate 20-byte outputs.
     *
     * For PRF-384 we need ceil(384/160) = 3 iterations → 60 bytes,
     * but we only use the first 48 bytes.
     */
    uint8_t concat[256]; /* label + 1 + data + 1 — should fit */
    size_t concat_len = label_len + 1 + data_len + 1;

    /* Build: label || 0x00 || data || i (i filled per iteration) */
    crypto_memcpy(concat, label, label_len);
    concat[label_len] = 0x00;
    crypto_memcpy(concat + label_len + 1, data, data_len);

    uint8_t hmac_out[20];
    size_t offset = 0;

    for (uint8_t i = 0; i < 3; i++) {
        concat[concat_len - 1] = i;
        iwn_hmac_sha1(key, 32, concat, concat_len, hmac_out);

        size_t copy_len = 48 - offset;
        if (copy_len > 20) copy_len = 20;
        crypto_memcpy(output + offset, hmac_out, copy_len);
        offset += copy_len;
    }
}
