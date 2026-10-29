// TailTiny's relay (DERP) TLS check (ml_derp_tls.h) against a local TLS server, on the Mbed TLS
// version ESP-IDF 6.1 ships (4.1.0). Opt-in: cmake -DHG_TLS_TESTS=ON downloads and builds it;
// firmware/CMakeLists.txt, hg_tailtiny_tls_tests.
//
// The certificates are test-only, P-256, valid until 2126, made with OpenSSL: a CA, a relay
// certificate it signed for derp.test, and a self-signed one for the same name.
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#include "check.hpp"

extern "C" {
#include "ml_derp_tls.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
}

namespace {

const char kCa[] = R"(-----BEGIN CERTIFICATE-----
MIIBqDCCAU2gAwIBAgIUIiVb901BDdtkQsIIBX7JHqfZJjkwCgYIKoZIzj0EAwIw
IDEeMBwGA1UEAwwVSGVybWVzIGdhZGdldCB0ZXN0IENBMCAXDTI2MTAyOTE1MDkz
NloYDzIxMjYxMDA1MTUwOTM2WjAgMR4wHAYDVQQDDBVIZXJtZXMgZ2FkZ2V0IHRl
c3QgQ0EwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAAQ+cNQPsqp0Nt2AV0cN8k5g
Eo5w4QgqSk1/0g2+zbOuGFp4A3Y1xDPtjddNd6L/x7kKBMs5NmfgOAtetZNJi6sS
o2MwYTAdBgNVHQ4EFgQU1VX1pBI8JHpOB5Hjd4KXg4Nz09swHwYDVR0jBBgwFoAU
1VX1pBI8JHpOB5Hjd4KXg4Nz09swDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8E
BAMCAQYwCgYIKoZIzj0EAwIDSQAwRgIhAJafOScgmzB+fBAO+2oYuNa3/zk8xv8Y
VmHtlsnyLaJSAiEAhXJAHtkVMe1RxMlpCDXMi4vpsUpnRq3TgAqWgXVoAMc=
-----END CERTIFICATE-----
)";
const char kRelayCert[] = R"(-----BEGIN CERTIFICATE-----
MIIBwDCCAWWgAwIBAgIUD8OJtPeK3fu2WjRFwMltDLSJcb4wCgYIKoZIzj0EAwIw
IDEeMBwGA1UEAwwVSGVybWVzIGdhZGdldCB0ZXN0IENBMCAXDTI2MTAyOTE1MDkz
NloYDzIxMjYxMDA1MTUwOTM2WjAUMRIwEAYDVQQDDAlkZXJwLnRlc3QwWTATBgcq
hkjOPQIBBggqhkjOPQMBBwNCAAQEq8Kekl3yczglfqK0zBnF6QZSxHrNQFRXLBdW
bce2zMF3Dc2vBznFCq0/2UNdh7QFhQIQTzKWADYpZjkKvkAio4GGMIGDMBQGA1Ud
EQQNMAuCCWRlcnAudGVzdDAJBgNVHRMEAjAAMAsGA1UdDwQEAwIHgDATBgNVHSUE
DDAKBggrBgEFBQcDATAdBgNVHQ4EFgQUUmfWl+PxXL9KvT5knNwmHu0YOhYwHwYD
VR0jBBgwFoAU1VX1pBI8JHpOB5Hjd4KXg4Nz09swCgYIKoZIzj0EAwIDSQAwRgIh
AKGdVLL4tQTXm1dlsNRcLXxYvx8JS+YZX+KqpGsIE4EsAiEAii5t4JPKsz/OQfaw
eutOd51e3BTOc9eNgkeTAUbfAvQ=
-----END CERTIFICATE-----
)";
const char kRelayKey[] = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgkNPKs6V/olKhSclk
NxkESiKpqdrzJ3G5nC55FpdPWBqhRANCAAQEq8Kekl3yczglfqK0zBnF6QZSxHrN
QFRXLBdWbce2zMF3Dc2vBznFCq0/2UNdh7QFhQIQTzKWADYpZjkKvkAi
-----END PRIVATE KEY-----
)";
const char kSelfCert[] = R"(-----BEGIN CERTIFICATE-----
MIIBlDCCATugAwIBAgIUJAe74VTBuhRlWKiHZ4SI1+G+p/AwCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJZGVycC50ZXN0MCAXDTI2MTAyOTE1MDkzNloYDzIxMjYxMDA1
MTUwOTM2WjAUMRIwEAYDVQQDDAlkZXJwLnRlc3QwWTATBgcqhkjOPQIBBggqhkjO
PQMBBwNCAATtcoFHiuXiQXR7bgy8tJMRPIMq61a01YwtlqRo+kl9AHMuIOhX3Wi7
qv6cqcILDkxXp4SOHnjvEh2WDIWlDvDCo2kwZzAdBgNVHQ4EFgQUNzF2oKQAoWuf
8m0tKk3mEgsudqQwHwYDVR0jBBgwFoAUNzF2oKQAoWuf8m0tKk3mEgsudqQwDwYD
VR0TAQH/BAUwAwEB/zAUBgNVHREEDTALgglkZXJwLnRlc3QwCgYIKoZIzj0EAwID
RwAwRAIgDXZxXbeLUvvuFJNK3uwIJE5QqrnixEkhKLQeertZg1sCIFz+6GvoAF3y
J42sJSD7EIThXtJhmtGgZicCCYLgLYpA
-----END CERTIFICATE-----
)";
const char kSelfKey[] = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg7cEbcWCxkSPIsoRR
8Knhae6JIUCE2icyoIYPy0rHEeehRANCAATtcoFHiuXiQXR7bgy8tJMRPIMq61a0
1YwtlqRo+kl9AHMuIOhX3Wi7qv6cqcILDkxXp4SOHnjvEh2WDIWlDvDC
-----END PRIVATE KEY-----
)";

mbedtls_x509_crt* test_ca() {
  static mbedtls_x509_crt ca;
  static bool parsed = false;
  if (!parsed) {
    mbedtls_x509_crt_init(&ca);
    parsed = mbedtls_x509_crt_parse(&ca, reinterpret_cast<const unsigned char*>(kCa), sizeof kCa) == 0;
  }
  return parsed ? &ca : nullptr;
}

// Stands in for esp_crt_bundle_attach(): trust the test CA.
int attach_test_ca(void* conf) {
  mbedtls_x509_crt* ca = test_ca();
  if (!ca) return -1;
  mbedtls_ssl_conf_ca_chain(static_cast<mbedtls_ssl_config*>(conf), ca, nullptr);
  return 0;
}

int attach_fails(void*) { return -1; }

int fd_send(void* ctx, const unsigned char* buf, size_t len) {
  ssize_t n = ::send(*static_cast<int*>(ctx), buf, len, MSG_NOSIGNAL);
  return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : static_cast<int>(n);
}

int fd_recv(void* ctx, unsigned char* buf, size_t len) {
  ssize_t n = ::recv(*static_cast<int*>(ctx), buf, len, 0);
  return n <= 0 ? MBEDTLS_ERR_SSL_CONN_EOF : static_cast<int>(n);
}

// A TLS server on one end of a socket pair, presenting cert/key, until the handshake ends.
void serve(int fd, const char* cert, size_t cert_len, const char* key, size_t key_len) {
  mbedtls_x509_crt crt;
  mbedtls_pk_context pk;
  mbedtls_ssl_config conf;
  mbedtls_ssl_context ssl;
  mbedtls_x509_crt_init(&crt);
  mbedtls_pk_init(&pk);
  mbedtls_ssl_config_init(&conf);
  mbedtls_ssl_init(&ssl);
  if (mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char*>(cert), cert_len) == 0 &&
      mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char*>(key), key_len, nullptr, 0) == 0 &&
      mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                  MBEDTLS_SSL_PRESET_DEFAULT) == 0 &&
      mbedtls_ssl_conf_own_cert(&conf, &crt, &pk) == 0 && mbedtls_ssl_setup(&ssl, &conf) == 0) {
    mbedtls_ssl_set_bio(&ssl, &fd, fd_send, fd_recv, nullptr);
    mbedtls_ssl_handshake(&ssl);
  }
  ::close(fd);
  mbedtls_ssl_free(&ssl);
  mbedtls_ssl_config_free(&conf);
  mbedtls_pk_free(&pk);
  mbedtls_x509_crt_free(&crt);
}

struct Result {
  int setup = 0;      // ml_derp_tls_verify()
  int handshake = 0;  // mbedtls_ssl_handshake() on the client
  uint32_t flags = 0; // mbedtls_ssl_get_verify_result()
};

// The relay connection's client side, set up as ml_derp_connect() does, against a local server.
Result connect(const char* host, const char* cert, size_t cert_len, const char* key, size_t key_len,
               int (*attach)(void*) = attach_test_ca) {
  Result r;
  CHECK_EQ(psa_crypto_init(), PSA_SUCCESS);
  int fds[2];
  CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  std::thread server(serve, fds[1], cert, cert_len, key, key_len);
  mbedtls_ssl_config conf;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config_init(&conf);
  mbedtls_ssl_init(&ssl);
  CHECK_EQ(mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT),
           0);
  r.setup = ml_derp_tls_verify(&conf, attach);
  if (r.setup == 0) {
    CHECK_EQ(mbedtls_ssl_setup(&ssl, &conf), 0);
    CHECK_EQ(mbedtls_ssl_set_hostname(&ssl, host), 0);
    mbedtls_ssl_set_bio(&ssl, &fds[0], fd_send, fd_recv, nullptr);
    r.handshake = mbedtls_ssl_handshake(&ssl);
    r.flags = mbedtls_ssl_get_verify_result(&ssl);
  }
  ::close(fds[0]);
  server.join();
  mbedtls_ssl_free(&ssl);
  mbedtls_ssl_config_free(&conf);
  return r;
}

}  // namespace

TEST("tailtiny tls: a relay certificate from a trusted CA, for the right host, is accepted") {
  Result r = connect("derp.test", kRelayCert, sizeof kRelayCert, kRelayKey, sizeof kRelayKey);
  CHECK_EQ(r.setup, 0);
  CHECK_EQ(r.handshake, 0);
  CHECK_EQ(r.flags, 0u);
}

TEST("tailtiny tls: a self-signed relay certificate is refused") {
  Result r = connect("derp.test", kSelfCert, sizeof kSelfCert, kSelfKey, sizeof kSelfKey);
  CHECK(r.handshake != 0);
  CHECK((r.flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0);
}

TEST("tailtiny tls: a trusted certificate for another host is refused") {
  Result r = connect("relay.example", kRelayCert, sizeof kRelayCert, kRelayKey, sizeof kRelayKey);
  CHECK(r.handshake != 0);
  CHECK((r.flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) != 0);
}

TEST("tailtiny tls: the connection isn't set up when the CA certificates can't be attached") {
  Result r = connect("derp.test", kRelayCert, sizeof kRelayCert, kRelayKey, sizeof kRelayKey, attach_fails);
  CHECK_EQ(r.setup, -1);
}
