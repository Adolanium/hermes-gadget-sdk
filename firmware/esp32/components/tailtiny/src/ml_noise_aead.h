/*
 * ChaCha20-Poly1305 for the Noise control channel, from the reference
 * implementation wireguard-lwip bundles. Mbed TLS 4 (ESP-IDF 6) made its own
 * ChaCha20-Poly1305 API private. Kept in a header so tests/host can check it.
 *
 * Tailscale's nonce is 4 zero bytes, then the counter big-endian
 * (Go: binary.BigEndian.PutUint64(n[4:], counter)). wireguard-lwip's nonce is
 * 4 zero bytes, then its 64-bit counter little-endian, so the counter is passed
 * byte-swapped.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "chacha20poly1305.h"

/* ciphertext layout: encrypted data (pt_len bytes), then the 16-byte tag */
static inline int noise_aead_encrypt(const uint8_t *key, uint64_t nonce,
                                     const uint8_t *ad, size_t ad_len,
                                     const uint8_t *plaintext, size_t pt_len,
                                     uint8_t *ciphertext) {
    chacha20poly1305_encrypt(ciphertext, plaintext, pt_len, ad, ad_len,
                             __builtin_bswap64(nonce), key);
    return 0;
}

/* Returns 0 if the tag is valid, -1 otherwise (including ct_len < 16); plaintext
 * is written only when the tag is valid. */
static inline int noise_aead_decrypt(const uint8_t *key, uint64_t nonce,
                                     const uint8_t *ad, size_t ad_len,
                                     const uint8_t *ciphertext, size_t ct_len,
                                     uint8_t *plaintext) {
    return chacha20poly1305_decrypt(plaintext, ciphertext, ct_len, ad, ad_len,
                                    __builtin_bswap64(nonce), key) ? 0 : -1;
}
