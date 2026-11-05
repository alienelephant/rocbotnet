// roclink's one platform primitive on the Raspberry Pi Pico: AES-128 encryption
// of a single block (roclink_secure.h, "The crypto layer"; built with
// -D ROCLINK_CRYPTO_CUSTOM). The ESP32 builds use its AES hardware instead,
// inside roclink, so this file is empty there.
//
// It comes from BearSSL, which arduino-pico already links for TLS: its
// constant-time AES ("ct"), as one CBC block with a zero IV, which is exactly
// one ECB block. roclink_sec_init_node_at() self-tests it against the AES
// test vectors before any frame is sealed, so a wrong result stops the link
// ("security setup failed") instead of going on the air.
#if defined(ARDUINO_ARCH_RP2040)
#include <bearssl/bearssl_block.h>
#include <roclink_secure.h>
#include <string.h>

extern "C" int roclink_aes128_encrypt(const uint8_t key[ROCLINK_KEY_LEN], const uint8_t in[ROCLINK_BLOCK_LEN],
                                      uint8_t out[ROCLINK_BLOCK_LEN]) {
    br_aes_ct_cbcenc_keys ctx;
    uint8_t iv[ROCLINK_BLOCK_LEN] = {0};
    br_aes_ct_cbcenc_init(&ctx, key, ROCLINK_KEY_LEN);
    memmove(out, in, ROCLINK_BLOCK_LEN);  // out may be in
    br_aes_ct_cbcenc_run(&ctx, iv, out, ROCLINK_BLOCK_LEN);
    roclink_crypto_wipe(&ctx, sizeof(ctx));  // the key schedule
    roclink_crypto_wipe(iv, sizeof(iv));     // now the ciphertext
    return 0;
}
#endif
