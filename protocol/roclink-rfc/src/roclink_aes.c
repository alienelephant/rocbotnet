/*
 * roclink_aes.c - the AES block for roclink_secure, from the platform: one
 * provider per platform, each compiled only when roclink_secure.h selects it
 * (see "The crypto layer" there). With none selected (ROCLINK_CRYPTO_EXTERNAL
 * or _CUSTOM), this file compiles to nothing and roclink_aes128_encrypt()
 * comes from the application.
 */
#include "roclink_secure.h"

/*
 * the AES block from the ESP32 family's hardware AES
 * engine, through ESP-IDF's driver. Selected automatically on ESP-IDF and
 * Arduino-ESP32 (2.x and 3.x).
 *
 * This is Espressif's own driver API (esp_aes_*), not mbedTLS, so it doesn't
 * change with the mbedTLS version a release ships.
 */
#if defined(ROCLINK_CRYPTO_ESP32)

#if defined(__has_include)
#if __has_include("aes/esp_aes.h")
#include "aes/esp_aes.h" /* ESP-IDF 4.3 and later */
#else
#include "esp32/aes.h" /* older ESP-IDF */
#endif
#else
#include "aes/esp_aes.h"
#endif

int roclink_aes128_encrypt(const uint8_t key[ROCLINK_KEY_LEN], const uint8_t in[ROCLINK_BLOCK_LEN],
                           uint8_t out[ROCLINK_BLOCK_LEN]) {
    esp_aes_context ctx;
    int rc;
    esp_aes_init(&ctx);
    rc = esp_aes_setkey(&ctx, key, 128);
    if (rc == 0) rc = esp_aes_crypt_ecb(&ctx, ESP_AES_ENCRYPT, in, out);
    esp_aes_free(&ctx);
    roclink_crypto_wipe(&ctx, sizeof(ctx)); /* the context holds a copy of the key */
    return rc == 0 ? 0 : -1;
}

#endif /* ROCLINK_CRYPTO_ESP32 */

/*
 * the AES block from the PSA Crypto API: automatic on
 * Zephyr, or -D ROCLINK_CRYPTO_PSA with TF-PSA-Crypto / mbedTLS linked
 * elsewhere.
 *
 * On Zephyr, enable CONFIG_MBEDTLS, CONFIG_MBEDTLS_PSA_CRYPTO_C (or another
 * PSA provider), PSA_WANT_KEY_TYPE_AES and PSA_WANT_ALG_ECB_NO_PADDING.
 */
#if defined(ROCLINK_CRYPTO_PSA)

#include <psa/crypto.h>

int roclink_aes128_encrypt(const uint8_t key[ROCLINK_KEY_LEN], const uint8_t in[ROCLINK_BLOCK_LEN],
                           uint8_t out[ROCLINK_BLOCK_LEN]) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t id = 0;
    uint8_t tmp[ROCLINK_BLOCK_LEN];
    size_t n = 0;
    psa_status_t st;
    size_t i;

    if (psa_crypto_init() != PSA_SUCCESS) return -1; /* cheap after the first call */
    /* A volatile key that lives only for this one block. */
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECB_NO_PADDING);
    st = psa_import_key(&attr, key, ROCLINK_KEY_LEN, &id);
    if (st == PSA_SUCCESS) {
        st = psa_cipher_encrypt(id, PSA_ALG_ECB_NO_PADDING, in, ROCLINK_BLOCK_LEN, tmp, sizeof(tmp),
                                &n);
        (void)psa_destroy_key(id);
    }
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS || n != ROCLINK_BLOCK_LEN) {
        roclink_crypto_wipe(tmp, sizeof(tmp));
        return -1;
    }
    for (i = 0; i < ROCLINK_BLOCK_LEN; ++i) out[i] = tmp[i];
    roclink_crypto_wipe(tmp, sizeof(tmp));
    return 0;
}

#endif /* ROCLINK_CRYPTO_PSA */

/*
 * the AES block from OpenSSL 3 (libcrypto), for Linux
 * gateways and the host tests. Select with -D ROCLINK_CRYPTO_OPENSSL and
 * link -lcrypto.
 */
#if defined(ROCLINK_CRYPTO_OPENSSL)

#include <openssl/evp.h>

int roclink_aes128_encrypt(const uint8_t key[ROCLINK_KEY_LEN], const uint8_t in[ROCLINK_BLOCK_LEN],
                           uint8_t out[ROCLINK_BLOCK_LEN]) {
    uint8_t tmp[ROCLINK_BLOCK_LEN];
    int n = 0;
    int ok;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    /* One ECB block, no padding: exactly AES-Encrypt(key, in). */
    ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, NULL) == 1 &&
         EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
         EVP_EncryptUpdate(ctx, tmp, &n, in, (int)ROCLINK_BLOCK_LEN) == 1 &&
         n == (int)ROCLINK_BLOCK_LEN;
    EVP_CIPHER_CTX_free(ctx); /* also clears the key schedule */
    if (ok) {
        size_t i;
        for (i = 0; i < ROCLINK_BLOCK_LEN; ++i) out[i] = tmp[i];
    }
    roclink_crypto_wipe(tmp, sizeof(tmp));
    return ok ? 0 : -1;
}

#endif /* ROCLINK_CRYPTO_OPENSSL */

#if !defined(ROCLINK_CRYPTO_ESP32) && !defined(ROCLINK_CRYPTO_PSA) && !defined(ROCLINK_CRYPTO_OPENSSL)
typedef int roclink_aes_unused; /* ISO C forbids an empty translation unit */
#endif
