/*
 * Hermes gadget patch: how the relay (DERP) TLS connection checks the server,
 * split out of ml_derp.c so the host tests (firmware/tests/test_tailtiny_tls.cpp)
 * can run it against a local TLS server.
 */
#pragma once

#include "mbedtls/ssl.h"

/* Require the relay's certificate to chain to the CA certificates attach_ca()
 * adds (the firmware passes esp_crt_bundle_attach) and to match the host name
 * the caller sets with mbedtls_ssl_set_hostname(). Upstream skipped
 * verification. WireGuard already encrypts what the relay carries; this stops
 * a network in the middle from impersonating the relay and dropping or
 * delaying it unnoticed. Returns 0, or -1 when attach_ca() fails. */
static inline int ml_derp_tls_verify(mbedtls_ssl_config *conf, int (*attach_ca)(void *conf)) {
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    return attach_ca(conf) == 0 ? 0 : -1;
}
