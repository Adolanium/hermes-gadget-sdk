// TF-PSA-Crypto user config for hg_tailtiny_tls_tests (TF_PSA_CRYPTO_USER_CONFIG_FILE).
// The test runs its TLS server on a second thread, and both ends share PSA Crypto's key store
// and random generator. Mbed TLS 4.1 builds without locking by default; without it the two
// handshakes race, and about one run in a few hundred corrupts the heap or fails the handshake.
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
