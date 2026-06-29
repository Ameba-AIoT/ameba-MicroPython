// SPDX-License-Identifier: MIT
// machine.WDT port implementation for ameba-rtos (AmebaDplus).
//
// INCLUDEFILE — included by extmod/machine_wdt.c via
// MICROPY_PY_MACHINE_WDT_INCLUDEFILE.
//
// The Ameba WDT timeout range is 1..65535 ms.  Once enabled the WDT
// cannot be disabled by software (SDK wdt_api.c note), so there is no
// deinit().  Like rp2/esp32, a second construction is idempotent: it
// re-arms the (already running) WDT with the new timeout and returns the
// shared singleton.  This matters after a soft reset (Ctrl-D) — the WDT
// hardware keeps counting, and the new script must be able to obtain a
// handle to feed it, otherwise the board would be forced to hard reset.

#include "wdt_api.h"

typedef struct _machine_wdt_obj_t {
    mp_obj_base_t base;
} machine_wdt_obj_t;

static const machine_wdt_obj_t machine_wdt_obj = {{&machine_wdt_type}};

static machine_wdt_obj_t *mp_machine_wdt_make_new_instance(mp_int_t id, mp_int_t timeout_ms) {
    if (id != 0) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("WDT(%d) doesn't exist"), id);
    }

    // The SDK declares Timeout as u16 with range 1..65535 (ameba_wdg.h).
    if (timeout_ms < 1 || timeout_ms > 65535) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout out of range"));
    }

    // Idempotent: (re)configure and (re)enable.  WDG_Enable on an already
    // running watchdog is harmless, and watchdog_refresh runs right after
    // to avoid an immediate timeout on a shortened interval.
    watchdog_init((uint32_t)timeout_ms);
    watchdog_start();
    watchdog_refresh();

    return (machine_wdt_obj_t *)&machine_wdt_obj;
}

static void mp_machine_wdt_feed(machine_wdt_obj_t *self) {
    (void)self;
    watchdog_refresh();
}
