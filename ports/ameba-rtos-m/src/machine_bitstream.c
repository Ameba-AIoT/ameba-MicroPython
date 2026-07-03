// SPDX-License-Identifier: MIT
// machine.bitstream for ameba-rtos (AmebaDplus / RTL8721Dx).
//
// Timing uses the SysTick counter (CPU clock = 240 MHz, LOAD = 239999 for the
// FreeRTOS 1 kHz tick).  The RTL8721Dx KM4 does not implement DWT CYCCNT
// (DWT_CTRL.NOCYCCNT == 1), so SysTick is the only available cycle counter.
//
// GPIO is on the LSYS APB4 bus.  The HAL path (mp_hal_pin_write ->
// GPIO_WriteBit ROM) costs ~400 ns per toggle.  We write GPIO_DR directly
// to reduce each toggle to a single read-modify-write on the APB bus.
//
// Formula follows ports/rp2/machine_bitstream.c: subtract NS_CYCLES_OVERHEAD
// once per element and once more in the period term, giving 3 * OH total
// removed from the period.  OH = 4 calibrated on RTL8721Dx @ 240 MHz
// (inter-bit overhead ~= 12 cycles = 3 * 4).
//
// SysTick wrap: LOAD = 240000, longest wait ~= 300 cycles (1.25 us).  Wrap
// probability per wait = 0.125 %.  Simple unsigned subtraction (start - VAL)
// is used; on the rare wrap the loop exits one iteration early, which the
// WS2812 +-150 ns tolerance absorbs.

#include "py/runtime.h"
#include "py/mphal.h"
#include "extmod/modmachine.h"
#include "ameba_soc.h"

#if MICROPY_PY_MACHINE_BITSTREAM

#define NS_CYCLES_OVERHEAD (0)

void machine_bitstream_high_low(mp_hal_pin_obj_t pin, uint32_t *timing_ns,
    const uint8_t *buf, size_t len) {

    // Convert ns to SysTick ticks (rp2 formula: 3 * OH removed from period).
    uint32_t fcpu_mhz = SystemCoreClock / 1000000;
    for (size_t i = 0; i < 4; ++i) {
        timing_ns[i] = fcpu_mhz * timing_ns[i] / 1000;
        if (timing_ns[i] > (2 * NS_CYCLES_OVERHEAD)) {
            timing_ns[i] -= NS_CYCLES_OVERHEAD;
        } else {
            timing_ns[i] = 0;
        }
        if (i % 2 == 1) {
            timing_ns[i] += timing_ns[i - 1] - NS_CYCLES_OVERHEAD;
        }
    }

    // Configure pin as output before the atomic section so lazy gpio_init()
    // (which takes an RTOS lock) does not run with IRQs disabled.
    mp_hal_pin_output(pin);

    // Direct GPIO_DR access, bypassing the HAL chain (~400 ns on LSYS APB4).
    // PinName: bit[5] = port (0 = A, 1 = B), bits[4:0] = pin number.
    uint32_t bit_mask = (1u << (pin & 0x1Fu));
    volatile uint32_t *gpio_dr = ((pin >> 5) == 0)
        ? &GPIOA_BASE->PORT[0].GPIO_DR
        : &GPIOB_BASE->PORT[0].GPIO_DR;

    mp_uint_t atomic_state = MICROPY_BEGIN_ATOMIC_SECTION();

    for (size_t i = 0; i < len; ++i) {
        uint8_t b = buf[i];
        for (size_t j = 0; j < 8; ++j) {
            uint32_t *t = &timing_ns[b >> 6 & 2];
            uint32_t start = SysTick->VAL;
            *gpio_dr |= bit_mask;
            while ((start - SysTick->VAL) < t[0]) {}
            *gpio_dr &= ~bit_mask;
            b <<= 1;
            while ((start - SysTick->VAL) < t[1]) {}
        }
    }

    MICROPY_END_ATOMIC_SECTION(atomic_state);
}

#endif // MICROPY_PY_MACHINE_BITSTREAM
