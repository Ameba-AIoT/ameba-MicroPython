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

// Every per-chip mbedtls_config_*.h in the vendored SDK comments these out
// (inherited as-is from Realtek's own SDK default, not a deliberate Ameba
// decision), which makes mbedtls_x509_time_is_past()/is_future() (library/
// x509.c) unconditionally return 0 -- every TLS peer certificate is accepted
// regardless of its notBefore/notAfter, including already-expired ones.
// Matches esp32's CONFIG_MBEDTLS_HAVE_TIME(_DATE)=y and upstream
// extmod/mbedtls/mbedtls_config_common.h's default (both always-on).
// MBEDTLS_HAVE_TIME's own time()/gmtime_r() path is left as the standard
// one -- see machine_rtc.c's datetime() setter, which is this port's side
// of making that clock meaningful (mirrors esp32's machine_rtc.c
// settimeofday() call).
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE

// mbedtls's built-in mbedtls_ms_time() (library/platform_util.c) only has an
// implementation for Linux/Windows hosts; on bare metal it's `#error "No
// mbedtls_ms_time available"` unless this is defined and something supplies
// the function ourselves -- same reason rp2 needs its own mbedtls_ms_time()
// (ports/rp2/mbedtls/mbedtls_port.c). Ours lives in machine_rtc.c, next to
// the RTC/wall-clock code it shares a clock with.
#define MBEDTLS_PLATFORM_MS_TIME_ALT
