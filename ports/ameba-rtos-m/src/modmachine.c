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
#include "ameba_soc.h"      // PMU sleep framework + SOCPS_AONWakeReason + SLEEP_CG/PG
#include "ameba_backup_reg.h"  // BKUP_Set / BKUP_BIT_UARTBURN_BOOT for bootloader()
#include "machine_pin.h"
#include "machine_timer.h"
#include "modmachine.h"

// Reset cause values, aligned with esp32 for cross-port compatibility.
typedef enum {
    MP_PWRON_RESET = 1,
    MP_HARD_RESET,
    MP_WDT_RESET,
    MP_DEEPSLEEP_RESET,
    MP_SOFT_RESET
} reset_reason_t;

// Wake-source values returned by machine.wake_reason(); PIN_WAKE/TIMER_WAKE
// numbered to match esp32 for cross-port familiarity.
typedef enum {
    MP_WAKE_NONE = 0,
    MP_WAKE_PIN = 2,
    MP_WAKE_TIMER = 4
} wake_reason_t;

// MP_MACHINE_WAKE_IDLE/SLEEP/DEEPSLEEP come from modmachine.h (included
// above) so that machine_rtc.c's RTC.irq(wake=...) can see the same values.

#define MICROPY_PY_MACHINE_EXTRA_GLOBALS \
    { MP_ROM_QSTR(MP_QSTR_Pin),             MP_ROM_PTR(&machine_pin_type) }, \
    { MP_ROM_QSTR(MP_QSTR_Timer),           MP_ROM_PTR(&machine_timer_type) }, \
    { MP_ROM_QSTR(MP_QSTR_PWRON_RESET),     MP_ROM_INT(MP_PWRON_RESET) }, \
    { MP_ROM_QSTR(MP_QSTR_HARD_RESET),      MP_ROM_INT(MP_HARD_RESET) }, \
    { MP_ROM_QSTR(MP_QSTR_WDT_RESET),       MP_ROM_INT(MP_WDT_RESET) }, \
    { MP_ROM_QSTR(MP_QSTR_DEEPSLEEP_RESET), MP_ROM_INT(MP_DEEPSLEEP_RESET) }, \
    { MP_ROM_QSTR(MP_QSTR_SOFT_RESET),      MP_ROM_INT(MP_SOFT_RESET) }, \
    { MP_ROM_QSTR(MP_QSTR_RTC),             MP_ROM_PTR(&machine_rtc_type) }, \
    { MP_ROM_QSTR(MP_QSTR_wake_reason),     MP_ROM_PTR(&machine_wake_reason_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_PIN_WAKE),        MP_ROM_INT(MP_WAKE_PIN) }, \
    { MP_ROM_QSTR(MP_QSTR_TIMER_WAKE),      MP_ROM_INT(MP_WAKE_TIMER) }, \
    { MP_ROM_QSTR(MP_QSTR_IDLE),            MP_ROM_INT(MP_MACHINE_WAKE_IDLE) }, \
    { MP_ROM_QSTR(MP_QSTR_SLEEP),           MP_ROM_INT(MP_MACHINE_WAKE_SLEEP) }, \
    { MP_ROM_QSTR(MP_QSTR_DEEPSLEEP),       MP_ROM_INT(MP_MACHINE_WAKE_DEEPSLEEP) }, \

static mp_obj_t mp_machine_unique_id(void) {
    // EFUSE_GetUUID returns a 4-byte chip UUID via sys_api.h -> ... -> ameba_chipinfo.h.
    u32 uuid;
    EFUSE_GetUUID(&uuid);
    return mp_obj_new_bytes((byte *)&uuid, sizeof(uuid));
}

static mp_int_t mp_machine_reset_cause(void) {
    // BOOT_Reason() reads REG_LSYS_BOOT_REASON_SW & 0x7FF (ameba_reset.h).
    // Check SOFT first: machine.reset() sets a CPU-triggered system-reset bit
    // (IS_SYS_RESET -- named per physical core on AmebaDplus, per TrustZone
    // world on AmebaGreen2), which must not be misclassified as HARD_RESET.
    u32 reason = BOOT_Reason();
    if (IS_SYS_RESET(reason)) {
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

#if MICROPY_PY_MACHINE_BOOTLOADER
// machine.bootloader() — reboot into UART download mode.
// Sets the SW flag in backup register dword0[9] (BKUP_BIT_UARTBURN_BOOT)
// which the RTL8721Dx bootrom checks on cold/warm boot to enter the UART
// flash-download protocol instead of loading firmware.
MP_NORETURN void mp_machine_bootloader(size_t n_args, const mp_obj_t *args) {
    BKUP_Set(BKUP_REG0, BKUP_BIT_UARTBURN_BOOT);
    sys_reset();
    while (1) {
    }
}
#endif

static void mp_machine_idle(void) {
    MP_THREAD_GIL_EXIT();
    taskYIELD();
    MP_THREAD_GIL_ENTER();
}

static mp_obj_t mp_machine_get_freq(void) {
    return mp_obj_new_int(CPU_ClkGet());
}

// Setting the CPU frequency is not implemented: _CPU_ClkSet() on this SoC only
// toggles the clock source between CLK_KM4_PLL and CLK_KM4_XTAL (there is no
// API to pick an arbitrary frequency), and it has no confirmed caller
// anywhere in the amebadplus SDK to show it's safe post-boot -- only
// amebalite/amebasmart use it. Raise rather than silently no-op.
static void mp_machine_set_freq(size_t n_args, const mp_obj_t *args) {
    mp_raise_NotImplementedError(MP_ERROR_TEXT("machine.freq set"));
}

// Cached wake reason for lightsleep: set after vTaskDelay returns so that
// wake_reason() can report TIMER_WAKE even though CG sleep does not arm the
// AON timer (SOCPS_AONWakeReason() returns 0 after CG).
static mp_int_t mp_lightsleep_wake_reason = MP_WAKE_NONE;

// machine.wake_reason() — report what ended the most recent sleep.
// Two sources:
//   deepsleep wake (chip reset): read SOCPS_AONWakeReason() — AON register
//     survives PG reset and holds the actual wake event.
//   lightsleep wake (in-place): CG sleep does not arm the AON timer, so the
//     AON register stays 0; use the cached mp_lightsleep_wake_reason instead.
static mp_obj_t machine_wake_reason(void) {
    if (mp_machine_reset_cause() == MP_DEEPSLEEP_RESET) {
        uint32_t reason = SOCPS_AONWakeReason();
        mp_int_t r = MP_WAKE_NONE;
        if (reason & WAKEPIN_EVENT) {
            r = MP_WAKE_PIN;
        } else if (reason != 0) {
            r = MP_WAKE_TIMER;
        }
        return MP_OBJ_NEW_SMALL_INT(r);
    }
    return MP_OBJ_NEW_SMALL_INT(mp_lightsleep_wake_reason);
}
static MP_DEFINE_CONST_FUN_OBJ_0(machine_wake_reason_obj, machine_wake_reason);

// machine.lightsleep([time_ms]) — clock-gated sleep (SLEEP_CG): CPU clocks gated,
// RAM and peripherals retained, returns in place on wake.  Realised via the SDK
// PMU/tickless path: drop the OS wakelock and block this task so the FreeRTOS idle
// task can enter CG sleep; the tick deadline (time_ms) or any interrupt wakes us.
//
// NOTE (v1): close Wi-Fi/BLE before sleeping — an active radio holds its own
// wakelock and keeps the system out of sleep (see Phase 20 spec).
static void mp_machine_lightsleep(size_t n_args, const mp_obj_t *args) {
    mp_int_t ms = (n_args > 0) ? mp_obj_get_int(args[0]) : -1;
    if (n_args > 0 && ms < 0) {
        ms = 0;   // an explicit negative duration is a no-op sleep, not "sleep forever"
    }
    uint32_t prev_type = pmu_get_sleep_type();
    pmu_set_sleep_type(SLEEP_CG);
    pmu_set_sysactive_time(0);          // allow the idle task to sleep immediately
    MP_THREAD_GIL_EXIT();
    pmu_release_wakelock(PMU_OS);        // let FreeRTOS tickless idle enter CG sleep
    vTaskDelay((ms >= 0) ? pdMS_TO_TICKS((uint32_t)ms) : portMAX_DELAY);
    pmu_acquire_wakelock(PMU_OS);        // hold the system awake again
    // CG sleep does not arm the AON timer, so SOCPS_AONWakeReason() returns 0.
    // Cache the reason here so wake_reason() can return the correct value.
    mp_lightsleep_wake_reason = (ms >= 0) ? MP_WAKE_TIMER : MP_WAKE_NONE;
    MP_THREAD_GIL_ENTER();
    pmu_set_sleep_type(prev_type);       // restore the default (PG) sleep type
}

// machine.deepsleep([time_ms]) — power-gated sleep (SLEEP_PG).  Does NOT return:
// the AON timer (time_ms) wakes the chip through a full reset, after which
// reset_cause() reports DEEPSLEEP_RESET.  Without time_ms only an external event
// (reset, or a wake pin in v1.1) can wake it.
MP_NORETURN static void mp_machine_deepsleep(size_t n_args, const mp_obj_t *args) {
    mp_int_t ms = (n_args > 0) ? mp_obj_get_int(args[0]) : -1;
    pmu_set_sleep_type(SLEEP_PG);
    if (ms > 0) {
        // The AON timer tops out at PMU_DEVICE_TIMER_MAX_INTERVAL (30 s); clamp so a
        // larger request can't overflow the ms->tick conversion inside the PMU.
        if (ms > PMU_DEVICE_TIMER_MAX_INTERVAL) {
            ms = PMU_DEVICE_TIMER_MAX_INTERVAL;
        }
        // PG wake time is taken from pmu_get_sleep_time() (the sleep-time range), NOT
        // from pmu_set_wakeup_timer() — that drives the tickless-internal PMC timer
        // and would NOT wake a power-gated chip.  min == max gives a deterministic ms
        // wake; the KM0 PG handler arms the AON timer from this value.  Mirrors the
        // SDK at_tickps DSLP path.
        pmu_set_sleep_time_range((uint32_t)ms, (uint32_t)ms);
    }
    pmu_set_sysactive_time(0);
    MP_THREAD_GIL_EXIT();
    // pmu_ready_to_dsleep() requires BOTH the OS wakelock AND the deep wakelock to be
    // released; otherwise the PMU falls back to clock-gate and never power-gates or
    // resets (which, with the loop below, would hang the REPL).
    pmu_release_deepwakelock(PMU_OS);
    pmu_release_wakelock(PMU_OS);
    // System power-gates once idle; wake is a chip reset, so control never returns.
    // With no timed wake (ms <= 0) only an external reset (or a v1.1 wake pin) wakes it.
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
