/*
 * test_crypto.c — Host-runnable tests for iwn_crypto
 *
 * 13 test vectors from FIPS, RFC, and IEEE standards.
 * Build: cc -std=c11 -Wall -Wextra -O2 -o test_crypto test_crypto.c iwn_crypto.c
 */

#include <stdio.h>
#include <string.h>
#include "iwn_crypto.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name)  do { \
    tests_run++; \
    printf("  [%2d] %-30s ", tests_run, name); \
} while(0)

#define PASS()  do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static void hex_dump(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) printf("%02x", data[i]);
}

static int check_hex(const uint8_t *got, const char *expected_hex, size_t len) {
    char buf[256];
    for (size_t i = 0; i < len; i++)
        sprintf(buf + i * 2, "%02x", got[i]);
    buf[len * 2] = '\0';
    if (strcmp(buf, expected_hex) == 0) return 1;
    printf("\n    expected: %s\n    got:      %s\n", expected_hex, buf);
    return 0;
}

/* ── SHA-1 Tests ───────────────────────────────────────────────────── */

static void test_sha1_abc(void) {
    TEST("sha1_abc");
    uint8_t digest[20];
    iwn_sha1((const uint8_t *)"abc", 3, digest);
    if (check_hex(digest, "a9993e364706816aba3e25717850c26c9cd0d89d", 20))
        PASS();
    else
        FAIL("SHA-1(\"abc\")");
}

static void test_sha1_empty(void) {
    TEST("sha1_empty");
    uint8_t digest[20];
    iwn_sha1((const uint8_t *)"", 0, digest);
    if (check_hex(digest, "da39a3ee5e6b4b0d3255bfef95601890afd80709", 20))
        PASS();
    else
        FAIL("SHA-1(\"\")");
}

static void test_sha1_two_blocks(void) {
    TEST("sha1_two_blocks");
    /* "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" (56 bytes)
     * This forces two SHA-1 blocks due to padding */
    const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t digest[20];
    iwn_sha1((const uint8_t *)msg, 56, digest);
    if (check_hex(digest, "84983e441c3bd26ebaae4aa1f95129e5e54670f1", 20))
        PASS();
    else
        FAIL("SHA-1(56-byte)");
}

/* ── HMAC-SHA1 Tests ───────────────────────────────────────────────── */

static void test_hmac_sha1_case1(void) {
    TEST("hmac_sha1_case1");
    /* RFC 2202 Test Case 1: key = 0x0b * 20, data = "Hi There" */
    uint8_t key[20];
    memset(key, 0x0b, 20);
    uint8_t mac[20];
    iwn_hmac_sha1(key, 20, (const uint8_t *)"Hi There", 8, mac);
    if (check_hex(mac, "b617318655057264e28bc0b6fb378c8ef146be00", 20))
        PASS();
    else
        FAIL("HMAC-SHA1 case 1");
}

static void test_hmac_sha1_case2(void) {
    TEST("hmac_sha1_case2");
    /* RFC 2202 Test Case 2: key = "Jefe", data = "what do ya want for nothing?" */
    uint8_t mac[20];
    iwn_hmac_sha1((const uint8_t *)"Jefe", 4,
                  (const uint8_t *)"what do ya want for nothing?", 28, mac);
    if (check_hex(mac, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", 20))
        PASS();
    else
        FAIL("HMAC-SHA1 case 2");
}

static void test_hmac_sha1_case3(void) {
    TEST("hmac_sha1_case3");
    /* RFC 2202 Test Case 3: key = 0xAA * 20, data = 0xDD * 50 */
    uint8_t key[20], data[50];
    memset(key, 0xAA, 20);
    memset(data, 0xDD, 50);
    uint8_t mac[20];
    iwn_hmac_sha1(key, 20, data, 50, mac);
    if (check_hex(mac, "125d7342b9ac11cd91a39af48aa17b4f63f175d3", 20))
        PASS();
    else
        FAIL("HMAC-SHA1 case 3");
}

/* ── PBKDF2-SHA1 Tests ─────────────────────────────────────────────── */

static void test_pbkdf2_ieee1(void) {
    TEST("pbkdf2_ieee1");
    /* IEEE 802.11i: "password" / "IEEE" / 4096 iterations → 32 bytes */
    uint8_t output[32];
    iwn_pbkdf2_sha1((const uint8_t *)"password", 8,
                    (const uint8_t *)"IEEE", 4,
                    4096, output, 32);
    if (check_hex(output,
        "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", 32))
        PASS();
    else
        FAIL("PBKDF2 IEEE test 1");
}

static void test_pbkdf2_ieee2(void) {
    TEST("pbkdf2_ieee2");
    /* IEEE 802.11i: "ThisIsAPassword" / "ThisIsASSID" / 4096 → 32 bytes */
    uint8_t output[32];
    iwn_pbkdf2_sha1((const uint8_t *)"ThisIsAPassword", 15,
                    (const uint8_t *)"ThisIsASSID", 11,
                    4096, output, 32);
    if (check_hex(output,
        "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af", 32))
        PASS();
    else
        FAIL("PBKDF2 IEEE test 2");
}

/* ── AES-128 Tests ─────────────────────────────────────────────────── */

static void test_aes128_encrypt(void) {
    TEST("aes128_encrypt");
    /* FIPS 197 Appendix B */
    const uint8_t key[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
    };
    const uint8_t plain[16] = {
        0x32,0x43,0xf6,0xa8,0x88,0x5a,0x30,0x8d,
        0x31,0x31,0x98,0xa2,0xe0,0x37,0x07,0x34
    };
    const uint8_t expected[16] = {
        0x39,0x25,0x84,0x1d,0x02,0xdc,0x09,0xfb,
        0xdc,0x11,0x85,0x97,0x19,0x6a,0x0b,0x32
    };

    iwn_aes_ctx ctx;
    iwn_aes128_init(&ctx, key);
    uint8_t out[16];
    iwn_aes128_encrypt(&ctx, plain, out);

    if (memcmp(out, expected, 16) == 0)
        PASS();
    else {
        printf("\n    expected: "); hex_dump(expected, 16);
        printf("\n    got:      "); hex_dump(out, 16);
        printf("\n");
        FAIL("AES-128 encrypt FIPS 197 App B");
    }
}

static void test_aes128_decrypt(void) {
    TEST("aes128_decrypt");
    /* Roundtrip: encrypt then decrypt must return original plaintext */
    const uint8_t key[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
    };
    const uint8_t plain[16] = {
        0x32,0x43,0xf6,0xa8,0x88,0x5a,0x30,0x8d,
        0x31,0x31,0x98,0xa2,0xe0,0x37,0x07,0x34
    };

    iwn_aes_ctx ctx;
    iwn_aes128_init(&ctx, key);

    uint8_t enc[16], dec[16];
    iwn_aes128_encrypt(&ctx, plain, enc);
    iwn_aes128_decrypt(&ctx, enc, dec);

    if (memcmp(dec, plain, 16) == 0)
        PASS();
    else {
        printf("\n    expected: "); hex_dump(plain, 16);
        printf("\n    got:      "); hex_dump(dec, 16);
        printf("\n");
        FAIL("AES-128 roundtrip");
    }
}

static void test_aes128_nist(void) {
    TEST("aes128_nist");
    /* NIST SP 800-38A F.1.1: ECB-AES128 Encrypt */
    const uint8_t key[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
    };
    const uint8_t plain[16] = {
        0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,
        0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a
    };
    const uint8_t expected[16] = {
        0x3a,0xd7,0x7b,0xb4,0x0d,0x7a,0x36,0x60,
        0xa8,0x9e,0xca,0xf3,0x24,0x66,0xef,0x97
    };

    iwn_aes_ctx ctx;
    iwn_aes128_init(&ctx, key);
    uint8_t out[16];
    iwn_aes128_encrypt(&ctx, plain, out);

    if (memcmp(out, expected, 16) == 0)
        PASS();
    else {
        printf("\n    expected: "); hex_dump(expected, 16);
        printf("\n    got:      "); hex_dump(out, 16);
        printf("\n");
        FAIL("NIST SP 800-38A F.1.1");
    }
}

/* ── AES Key Unwrap Test ───────────────────────────────────────────── */

static void test_aes_unwrap(void) {
    TEST("aes_unwrap");
    /* RFC 3394 Section 4.1: 128-bit KEK, 128-bit key data */
    const uint8_t kek[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F
    };
    const uint8_t cipher[24] = {
        0x1F,0xA6,0x8B,0x0A,0x81,0x12,0xB4,0x47,
        0xAE,0xF3,0x4B,0xD8,0xFB,0x5A,0x7B,0x82,
        0x9D,0x3E,0x86,0x23,0x71,0xD2,0xCF,0xE5
    };
    const uint8_t expected_plain[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF
    };

    uint8_t plain[16];
    int ret = iwn_aes_unwrap(kek, cipher, 24, plain);

    if (ret == 0 && memcmp(plain, expected_plain, 16) == 0)
        PASS();
    else {
        if (ret != 0) {
            FAIL("integrity check failed");
        } else {
            printf("\n    expected: "); hex_dump(expected_plain, 16);
            printf("\n    got:      "); hex_dump(plain, 16);
            printf("\n");
            FAIL("AES unwrap data mismatch");
        }
    }
}

/* ── PRF-384 Test ──────────────────────────────────────────────────── */

static void test_prf384(void) {
    TEST("prf384");
    /*
     * Known-answer test: compute PRF-384 with a fixed key, label, and data,
     * then verify against a reference output computed step-by-step.
     *
     * key   = 32 bytes of 0x0b
     * label = "Pairwise key expansion" (22 bytes)
     * data  = 32 bytes: 0x00..0x1f
     *
     * We verify by computing the same thing manually:
     * concat = label || 0x00 || data || i
     * HMAC-SHA1(key, concat) for i=0,1,2
     */
    uint8_t key[32];
    memset(key, 0x0b, 32);

    const char *label_str = "Pairwise key expansion";
    size_t label_len = 22;

    uint8_t data[32];
    for (int i = 0; i < 32; i++) data[i] = (uint8_t)i;

    /* Compute expected: manual PRF */
    uint8_t concat[256];
    memcpy(concat, label_str, label_len);
    concat[label_len] = 0x00;
    memcpy(concat + label_len + 1, data, 32);
    size_t concat_len = label_len + 1 + 32 + 1; /* +1 for counter byte */

    uint8_t expected[48];
    uint8_t hmac_out[20];

    for (uint8_t i = 0; i < 3; i++) {
        concat[concat_len - 1] = i;
        iwn_hmac_sha1(key, 32, concat, concat_len, hmac_out);
        size_t copy = (i < 2) ? 20 : 8; /* 20+20+8 = 48 */
        memcpy(expected + i * 20, hmac_out, copy);
    }

    /* Now compute via the API */
    uint8_t output[48];
    iwn_prf384(key, (const uint8_t *)label_str, label_len, data, 32, output);

    if (memcmp(output, expected, 48) == 0)
        PASS();
    else {
        printf("\n    expected: "); hex_dump(expected, 48);
        printf("\n    got:      "); hex_dump(output, 48);
        printf("\n");
        FAIL("PRF-384");
    }
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("IWN Crypto Test Suite\n");
    printf("=====================\n\n");

    printf("SHA-1:\n");
    test_sha1_abc();
    test_sha1_empty();
    test_sha1_two_blocks();

    printf("\nHMAC-SHA1:\n");
    test_hmac_sha1_case1();
    test_hmac_sha1_case2();
    test_hmac_sha1_case3();

    printf("\nPBKDF2-SHA1:\n");
    test_pbkdf2_ieee1();
    test_pbkdf2_ieee2();

    printf("\nAES-128:\n");
    test_aes128_encrypt();
    test_aes128_decrypt();
    test_aes128_nist();

    printf("\nAES Key Unwrap:\n");
    test_aes_unwrap();

    printf("\nPRF-384:\n");
    test_prf384();

    printf("\n=====================\n");
    printf("Results: %d/%d passed\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
