// The SDK's mbedtls (ameba-rtos/component/ssl/mbedtls-3.6.2) prints full
// English descriptions from mbedtls_strerror() (e.g. "SSL - Invalid value in
// SSL config"). Upstream MicroPython tests (extmod/tls_sslcontext_ciphers.py,
// net_hosted/ssl_verify_callback.py, net_inet/tls_text_errors.py) expect the
// space-optimized error #define names instead (e.g.
// "MBEDTLS_ERR_SSL_BAD_CONFIG") -- see micropython/lib/mbedtls_errors/. esp32
// gets this via esp32_mbedtls_errors.c; this port has no equivalent because it
// links the SDK's own prebuilt mbedtls rather than micropython/lib/mbedtls.
//
// We can't just add mp_mbedtls_errors.c as its own translation unit: the SDK
// links every component archive with -Wl,--whole-archive, so its
// mbedtls_strerror (in lib_mbedtls.a's error.o) and ours would both be pulled
// in unconditionally, causing a "multiple definition" link error. Renaming
// ours to __wrap_mbedtls_strerror and pairing it with -Wl,-wrap,mbedtls_strerror
// (see CMakeLists.txt) redirects callers to our version while leaving the
// SDK's still-linked-but-now-uncalled definition alone.
#define mbedtls_strerror __wrap_mbedtls_strerror
#include "../../../micropython/lib/mbedtls_errors/mp_mbedtls_errors.c"
