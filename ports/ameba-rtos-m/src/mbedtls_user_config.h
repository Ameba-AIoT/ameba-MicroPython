// mbedtls "user config" override (see mbedtls/build_info.h): included right
// after the SDK's MBEDTLS_CONFIG_FILE to override its defaults for our build.
// Registered via ameba_global_define() (not just our own target's private
// definitions) so this also applies when the mbedtls library itself is
// compiled from source -- required for MBEDTLS_SSL_DTLS_HELLO_VERIFY below,
// whose actual implementation lives in the library, not just our glue code.
//
// ameba-rtos's mbedtls_config.h unconditionally defines MBEDTLS_DEBUG_C. That
// makes upstream extmod/modtls_mbedtls.c call mbedtls_debug_set_threshold(3)
// and register a debug callback on every TLS connection, flooding the UART
// with per-byte handshake dumps. Under sustained load this was observed to
// crash the board (py/mpprint.c "unsupported fmt char" assertion) mid
// handshake. Undefining it here keeps modtls_mbedtls.c's #ifdef MBEDTLS_DEBUG_C
// block from registering the callback, so mbedtls's debug calls stay no-ops.
#undef MBEDTLS_DEBUG_C

// mbedtls_config_green2.h comments this out, so extmod/modtls_mbedtls.c's
// client_id kwarg (needed for tls.PROTOCOL_DTLS_SERVER) is compiled away and
// mbedtls_ssl_set_client_transport_id()/mbedtls_ssl_conf_dtls_cookies() don't
// exist in lib_mbedtls.a at all (ssl_tls12_server.c wraps them in the same
// #if). Requires MBEDTLS_SSL_PROTO_DTLS, which is already enabled.
#define MBEDTLS_SSL_DTLS_HELLO_VERIFY
