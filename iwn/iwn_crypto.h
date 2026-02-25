/*
 * iwn_crypto.h — WPA2-PSK crypto primitives for IWN WiFi driver
 *
 * Self-contained: only depends on <stdint.h> and <stddef.h>.
 * No UEFI headers, no libc. Host-testable and cross-compilable.
 *
 * Implements: SHA-1, HMAC-SHA1, PBKDF2-SHA1, AES-128, AES Key Unwrap, PRF-384
 */
#ifndef IWN_CRYPTO_H
#define IWN_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

/* ── SHA-1 (FIPS 180-4) ──────────────────────────────────────────────── */

typedef struct {
    uint32_t state[5];
    uint8_t  buf[64];
    uint32_t buf_len;
    uint64_t total_len;
} iwn_sha1_ctx;

void iwn_sha1_init(iwn_sha1_ctx *ctx);
void iwn_sha1_update(iwn_sha1_ctx *ctx, const uint8_t *data, size_t len);
void iwn_sha1_final(iwn_sha1_ctx *ctx, uint8_t digest[20]);

/* One-shot convenience */
void iwn_sha1(const uint8_t *data, size_t len, uint8_t digest[20]);

/* ── HMAC-SHA1 (RFC 2104) ────────────────────────────────────────────── */

void iwn_hmac_sha1(const uint8_t *key, size_t key_len,
                   const uint8_t *data, size_t data_len,
                   uint8_t mac[20]);

/* ── PBKDF2-SHA1 (RFC 2898) ──────────────────────────────────────────── */

void iwn_pbkdf2_sha1(const uint8_t *password, size_t password_len,
                     const uint8_t *salt, size_t salt_len,
                     uint32_t iterations,
                     uint8_t *output, size_t output_len);

/* ── AES-128 (FIPS 197) ──────────────────────────────────────────────── */

typedef struct {
    uint32_t rk[44]; /* Expanded round keys */
} iwn_aes_ctx;

void iwn_aes128_init(iwn_aes_ctx *ctx, const uint8_t key[16]);
void iwn_aes128_encrypt(const iwn_aes_ctx *ctx,
                        const uint8_t in[16], uint8_t out[16]);
void iwn_aes128_decrypt(const iwn_aes_ctx *ctx,
                        const uint8_t in[16], uint8_t out[16]);

/* ── AES Key Unwrap (RFC 3394) ────────────────────────────────────────
 *
 * cipher_len must be a multiple of 8 and >= 16.
 * plain buffer must hold cipher_len - 8 bytes.
 * Returns 0 on success, -1 on integrity check failure.
 */
int iwn_aes_unwrap(const uint8_t kek[16],
                   const uint8_t *cipher, size_t cipher_len,
                   uint8_t *plain);

/* ── PRF-384 (IEEE 802.11i) ──────────────────────────────────────────
 *
 * Produces 48 bytes of keying material.
 * Used to derive PTK from PMK + addresses + nonces.
 */
void iwn_prf384(const uint8_t key[32],
                const uint8_t *label, size_t label_len,
                const uint8_t *data, size_t data_len,
                uint8_t output[48]);

#endif /* IWN_CRYPTO_H */
