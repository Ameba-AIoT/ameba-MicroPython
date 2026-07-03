/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Realtek Corporation
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

#ifndef MICROPY_INCLUDED_AMEBA_MPHALPORT_H
#define MICROPY_INCLUDED_AMEBA_MPHALPORT_H

#include <stdint.h>
#include "PinNames.h"
#include "py/ringbuf.h"

// Exposed for os.dupterm_notify (modos.c needs to push bytes into stdin).
extern ringbuf_t stdin_ringbuf;

// Save PRIMASK and disable all interrupts; return saved state for restoration.
static inline uint32_t mp_hal_begin_atomic_section(void) {
    uint32_t state;
    __asm volatile ("mrs %0, primask\n\t cpsid i" : "=r" (state) :: "memory");
    return state;
}

// Restore PRIMASK to the value saved by mp_hal_begin_atomic_section().
static inline void mp_hal_end_atomic_section(uint32_t state) {
    __asm volatile ("msr primask, %0" :: "r" (state) : "memory");
}

#define MICROPY_BEGIN_ATOMIC_SECTION()     mp_hal_begin_atomic_section()
#define MICROPY_END_ATOMIC_SECTION(state)  mp_hal_end_atomic_section(state)

// Pin HAL type.  Must be a #define (not typedef) so that py/mphal.h sees it
// is already defined and skips its fallback macro, which would override our
// mp_hal_get_pin_obj function declaration below.
#define mp_hal_pin_obj_t PinName

// Used by machine.PWM and other pin-based modules to extract the PinName
// from a machine.Pin object.  Implemented in machine_pin.c.
mp_hal_pin_obj_t mp_hal_get_pin_obj(void *pin_obj);

// Resolve a user pin argument (Pin object, int PinName, or board string) to a
// validated PinName, raising ValueError on an unknown pin.  Implemented in
// machine_pin.c.  Used by machine.I2S, which accepts raw user pin arguments.
mp_hal_pin_obj_t mp_hal_pin_resolve(void *pin_in);

// HAL pin primitives used by machine.SoftI2C / machine.SoftSPI /
// machine.time_pulse_us.  Implemented in machine_pin.c.
void mp_hal_pin_input(mp_hal_pin_obj_t pin);
void mp_hal_pin_output(mp_hal_pin_obj_t pin);
void mp_hal_pin_open_drain(mp_hal_pin_obj_t pin);
int mp_hal_pin_read(mp_hal_pin_obj_t pin);
void mp_hal_pin_write(mp_hal_pin_obj_t pin, int v);
void mp_hal_pin_od_low(mp_hal_pin_obj_t pin);
void mp_hal_pin_od_high(mp_hal_pin_obj_t pin);

// Format/name helpers used by extmod machine.I2S.
#define MP_HAL_PIN_FMT  "%s"

// Return a human-readable pin name.  Rotates through a small ring of static
// buffers so that multiple calls within a single printf argument list (e.g.
// machine.I2S print, which formats sck/ws/sd/mck in one mp_printf) each get a
// distinct buffer instead of all aliasing one — otherwise every "%s" would
// show the last-evaluated pin name.  Single-threaded use only.
static inline const char *mp_hal_pin_name(mp_hal_pin_obj_t pin) {
    #define MP_HAL_PIN_NAME_NBUF  4
    static char bufs[MP_HAL_PIN_NAME_NBUF][8];
    static unsigned int next;
    char *buf = bufs[next++ % MP_HAL_PIN_NAME_NBUF];
    const char *port = (pin < PB_0) ? "PA" : "PB";
    int num = (pin < PB_0) ? (int)pin : (int)(pin - PB_0);
    buf[0] = port[0];
    buf[1] = port[1];
    if (num >= 10) { buf[2] = '0' + num / 10; buf[3] = '0' + num % 10; buf[4] = '\0'; }
    else { buf[2] = '0' + num; buf[3] = '\0'; }
    return buf;
}

mp_uint_t mp_hal_ticks_us(void);

// machine.SoftI2C uses the "fast" delay variant; we have no lower-overhead path
// than mp_hal_delay_us, so alias it.
#define mp_hal_delay_us_fast(us) mp_hal_delay_us(us)

// DHT driver needs quiet-timing guards (disable/restore interrupts around
// the bit-bang timing loop to prevent jitter from preemption).
#define mp_hal_quiet_timing_enter() mp_hal_begin_atomic_section()
#define mp_hal_quiet_timing_exit(v) mp_hal_end_atomic_section(v)

void mp_hal_set_interrupt_char(int c);
void mp_hal_get_random(size_t n, void *buf);

#endif // MICROPY_INCLUDED_AMEBA_MPHALPORT_H
