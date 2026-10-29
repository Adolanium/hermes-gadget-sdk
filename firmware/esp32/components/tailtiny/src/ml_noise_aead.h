/*
 * Hermes gadget patch: the Noise control channel's ChaCha20-Poly1305, split out
 * of ml_noise.c so the host tests (firmware/tests/test_tailtiny_aead.cpp) can
 * check it against RFC 8439 and Tailscale's nonce layout.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "chacha20poly1305.h"

/* Hermes gadget patch: Mbed TLS 4 (ESP-IDF 6) made its ChaCha20-Poly1305 API
 * private, so use the reference implementation that wireguard-lwip bundles.
 * Tailscale's nonce is 4 zero bytes then a BIG-endian counter (Go:
 * binary.BigEndian.PutUint64(n[4:], counter)); the reference code writes its
 * 64-bit nonce little-endian into those same 8 bytes, so pass it byte-swapped. */
static inline int noise_aead_encrypt(const uint8_t *key, uint64_t nonce,
                                     const uint8_t *ad, size_t ad_len,
                                     const uint8_t *plaintext, size_t pt_len,
                                     uint8_t *ciphertext) {
    /* ciphertext layout: encrypted_data(pt_len) + tag(16) */
    chacha20poly1305_encrypt(ciphertext, plaintext, pt_len, ad, ad_len,
                             __builtin_bswap64(nonce), key);
    return 0;
}

static inline int noise_aead_decrypt(const uint8_t *key, uint64_t nonce,
                                     const uint8_t *ad, size_t ad_len,
                                     const uint8_t *ciphertext, size_t ct_len,
                                     uint8_t *plaintext) {
    if (ct_len < 16) return -1;
    return chacha20poly1305_decrypt(plaintext, ciphertext, ct_len, ad, ad_len,
                                    __builtin_bswap64(nonce), key) ? 0 : -1;
}
