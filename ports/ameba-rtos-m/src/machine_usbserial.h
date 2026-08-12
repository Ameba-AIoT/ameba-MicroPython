// ports/ameba-rtos-m/src/machine_usbserial.h
//
// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "py/obj.h"

extern const mp_obj_type_t machine_usbserial_type;

// Brings up the USB core + CDC-ACM class driver. Called once from
// mp_main.c before the first soft_reset (unconditionally for EV8711FLM
// builds) so the REPL mirror in mphalport.c works from first boot, without
// requiring machine.USBSerial() to be constructed first. Safe to call again
// (e.g. from USBSerial.init()) -- no-ops if already up.
void machine_usbserial_init0(void);

// HAL-layer hooks consumed by mphalport.c, called *alongside* (not through)
// os.dupterm -- see mphalport.c's mp_hal_stdio_poll/mp_hal_stdin_rx_chr/
// mp_hal_stdout_tx_strn for the call sites this pairs with.
uintptr_t mp_usbserial_poll(uintptr_t poll_flags);
int mp_usbserial_rx_chr(void);
void mp_usbserial_tx_strn(const char *str, size_t len);
