// ports/ameba-rtos-m/src/modmachine.h
//
// Cross-translation-unit constants owned by modmachine.c but consumed by
// other standalone port .c files. modmachine.c is only ever #include'd into
// extmod/modmachine.c (MICROPY_PY_MACHINE_INCLUDEFILE), so anything #define'd
// directly inside it is invisible to every other .c file in this port --
// this header is the seam that makes a subset of it visible again.
#pragma once

// Generic upstream sleep-mode constants for RTC.irq(wake=...), aligned with
// esp32/pyboard numbering for cross-port familiarity. None of them are
// currently functional as a real wake source for RTC.irq() (see
// machine_rtc.c): confirmed on hardware on both boards that an armed RTC
// alarm does not wake a parameterless machine.lightsleep() (it hangs
// forever, recoverable only by re-flashing), and deepsleep wake sources are
// separately limited to the AON timer / AON GPIO pins. See
// ports/doc/quickref.rst "RTC alarm and interrupts" for the user-facing
// writeup and workaround.
#define MP_MACHINE_WAKE_IDLE      (1)
#define MP_MACHINE_WAKE_SLEEP     (2)
#define MP_MACHINE_WAKE_DEEPSLEEP (4)
