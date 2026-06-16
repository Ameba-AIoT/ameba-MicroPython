/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013-2023 Damien P. George
 * Copyright (c) 2016 Paul Sokolovsky
 * Copyright (c) 2024 Realtek Semiconductor Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// This file is never compiled standalone, it's included directly from
// extmod/modmachine.c via MICROPY_PY_MACHINE_INCLUDEFILE.
#include "FreeRTOS.h"
#include "task.h"
#include "py/mpthread.h"
#include "sys_api.h"
#include "machine_pin.h"
#include "machine_timer.h"

// Reset cause values, aligned with esp32 for cross-port compatibility.
typedef enum {
    MP_PWRON_RESET = 1,
    MP_HARD_RESET,
    MP_WDT_RESET,
    MP_DEEPSLEEP_RESET,
    MP_SOFT_RESET
} reset_reason_t;

#define MICROPY_PY_MACHINE_EXTRA_GLOBALS \
    { MP_ROM_QSTR(MP_QSTR_Pin),             MP_ROM_PTR(&machine_pin_type) }, \
    { MP_ROM_QSTR(MP_QSTR_Timer),           MP_ROM_PTR(&machine_timer_type) }, \
    { MP_ROM_QSTR(MP_QSTR_PWRON_RESET),     MP_ROM_INT(MP_PWRON_RESET) }, \
    { MP_ROM_QSTR(MP_QSTR_HARD_RESET),      MP_ROM_INT(MP_HARD_RESET) }, \
    { MP_ROM_QSTR(MP_QSTR_WDT_RESET),       MP_ROM_INT(MP_WDT_RESET) }, \
    { MP_ROM_QSTR(MP_QSTR_DEEPSLEEP_RESET), MP_ROM_INT(MP_DEEPSLEEP_RESET) }, \
    { MP_ROM_QSTR(MP_QSTR_SOFT_RESET),      MP_ROM_INT(MP_SOFT_RESET) }, \

static mp_obj_t mp_machine_unique_id(void) {
    // EFUSE_GetUUID returns a 4-byte chip UUID via sys_api.h -> ... -> ameba_chipinfo.h.
    u32 uuid;
    EFUSE_GetUUID(&uuid);
    return mp_obj_new_bytes((byte *)&uuid, sizeof(uuid));
}

static mp_int_t mp_machine_reset_cause(void) {
    // BOOT_Reason() reads REG_LSYS_BOOT_REASON_SW & 0x7FF (ameba_reset.h).
    // Check SOFT first: machine.reset() sets KM4_SYS_RST, which must not be
    // misclassified as HARD_RESET.
    u32 reason = BOOT_Reason();
    if (reason & (AON_BIT_RSTF_KM4_SYS | AON_BIT_RSTF_KM0_SYS)) {
        return MP_SOFT_RESET;
    }
    if (IS_WDG_RESET(reason)) {
        return MP_WDT_RESET;
    }
    if (reason & AON_BIT_RSTF_DSLP) {
        return MP_DEEPSLEEP_RESET;
    }
    if (reason & AON_BIT_RSTF_BOR) {
        return MP_PWRON_RESET;
    }
    return MP_HARD_RESET;
}

MP_NORETURN static void mp_machine_reset(void) {
    sys_reset();
    while (1) {
    }
}

static void mp_machine_idle(void) {
    MP_THREAD_GIL_EXIT();
    taskYIELD();
    MP_THREAD_GIL_ENTER();
}

// Stubs for MICROPY_PY_MACHINE_BARE_METAL_FUNCS — freq/sleep not yet implemented.
static mp_obj_t mp_machine_get_freq(void) {
    mp_raise_NotImplementedError(MP_ERROR_TEXT("machine.freq"));
}

static void mp_machine_set_freq(size_t n_args, const mp_obj_t *args) {
    mp_raise_NotImplementedError(MP_ERROR_TEXT("machine.freq"));
}

static void mp_machine_lightsleep(size_t n_args, const mp_obj_t *args) {
    mp_raise_NotImplementedError(MP_ERROR_TEXT("machine.lightsleep"));
}

MP_NORETURN static void mp_machine_deepsleep(size_t n_args, const mp_obj_t *args) {
    mp_raise_NotImplementedError(MP_ERROR_TEXT("machine.deepsleep"));
}
