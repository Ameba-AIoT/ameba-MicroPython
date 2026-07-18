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

#include <stdio.h>
#include <sys/time.h>

#include "ameba.h"
#include "os_wrapper.h"
#include "mpconfigport.h"
#include "py/ringbuf.h"
#include "shared/runtime/interrupt_char.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "extmod/misc.h"

static uint8_t uart_ringbuf_array[256];
// SPSC: log_uart_irq (ISR) is the sole producer; mp_hal_stdin_rx_chr (task) is the sole consumer.
ringbuf_t stdin_ringbuf = {uart_ringbuf_array, sizeof(uart_ringbuf_array), 0, 0};

// Counts bytes dropped when the RX ring buffer is full.
static volatile uint32_t uart_rx_overflow;

uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags) {
    uintptr_t ret = 0;
    #if MICROPY_PY_OS_DUPTERM
    ret |= mp_os_dupterm_poll(poll_flags);
    #endif
    // Check ringbuffer directly for uart and usj.
    if ((poll_flags & MP_STREAM_POLL_RD) && ringbuf_peek(&stdin_ringbuf) != -1) {
        ret |= MP_STREAM_POLL_RD;
    }
    if (poll_flags & MP_STREAM_POLL_WR) {
        ret |= MP_STREAM_POLL_WR;
    }
    return ret;
}

int mp_hal_stdin_rx_chr(void) {
    //DiagPrintfNano("%s %x\r\n", __FUNCTION__, __builtin_return_address(0));
    for (;;) {
        int c = ringbuf_get(&stdin_ringbuf);
        if (c != -1) {
            return c;
        }
        #if MICROPY_PY_OS_DUPTERM
        int dupterm_c = mp_os_dupterm_rx_chr();
        if (dupterm_c >= 0) {
            return dupterm_c;
        }
        #endif
        MICROPY_EVENT_POLL_HOOK
    }
}

extern void LOGUART_PutChar_RAM(u8 c);
mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len)
{
    #if MICROPY_PY_OS_DUPTERM
    // Mirror REPL output to any dupterm streams (WebREPL, second console, ...).
    // Called before the LOGUART loop, which consumes str/len.
    mp_os_dupterm_tx_strn(str, len);
    #endif
	mp_uint_t nChars = 0;
    //DiagPrintfNano("%s %x\r\n", __FUNCTION__, __builtin_return_address(0));
	for (/*Empty */; len > 0; --len) {
		LOGUART_PutChar_RAM(*str++);
		++nChars;
	}
	return nChars;
}


mp_uint_t mp_hal_ticks_ms(void) {
    return rtos_time_get_current_system_time_ms_64bit();
}

// The debug timer (DTIM) is a free-running 1 MHz counter, clocked at boot by
// the bootloader (APBPeriph_DTIM is enabled in bootloader_km4tz.c alongside
// LOGUART/thermal/USB) -- no init needed on our side.  Unlike
// rtos_time_get_current_system_time_us() (which combines the FreeRTOS tick
// count with a SysTick sub-tick read inside a critical section), this is a
// single register read with no critical section and no dependency on the
// RTOS tick, giving 1 us resolution unaffected by SysTick-related latency.
// Already used by the SDK's own SD card / Ethernet drivers for timeouts.
mp_uint_t mp_hal_ticks_us(void) {
    return DTimestamp_Get();
}
// DWT cycle counter for sub-microsecond timing (machine.bitstream / neopixel).
// RTL8721Dx KM4 runs at 200 MHz → 1 cycle = 5 ns, well within the ±150 ns
// WS2812 tolerance.  The counter must be explicitly enabled after every cold
// boot; it resets to 0 on power-on but the TRCENA and CYCCNTENA enable bits
// are cleared, so the register reads 0 without this initialisation.
void mp_hal_dwt_init(void) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;  // enable trace subsystem
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;            // start cycle counter
}

mp_uint_t mp_hal_ticks_cpu(void) {
    return DWT->CYCCNT;
}

uint64_t mp_hal_time_ns(void) {
    return rtos_time_get_current_system_time_ns();
}

// ---------------------------------------------------------------------------
// _gettimeofday()/_settimeofday() -- wall-clock syscalls
// ---------------------------------------------------------------------------
// AmebaGreen2's build links component/soc/amebagreen2/swlib/libnosys/
// gettod.c (bundled straight into lib_swlib.a), which already provides a
// real _gettimeofday()/_settimeofday() pair -- confirmed by a "multiple
// definition" link error when this was first written unconditionally.
// AmebaDplus's build does NOT link the equivalent
// component/soc/amebadplus/swlib/libnosys/gettod.c (confirmed via the link
// map: _gettimeofday resolves to the toolchain's own -specs nosys.specs
// libnosys.a(gettod.o) stub instead, and nothing at all satisfies
// _settimeofday), so this port must supply its own pair there.
// time.time()/gettimeofday(), machine.RTC()'s datetime() setter, and
// mbedtls_ms_time() (mbedtls_user_config.h's MBEDTLS_PLATFORM_MS_TIME_ALT --
// see machine_rtc.c) all depend on one of these two being real.
#if defined(CONFIG_AMEBADPLUS)
// wall_clock_base_ms/_tick_ms are re-based on every call (read or write),
// not just on _settimeofday(), so the mp_hal_ticks_ms() delta between
// updates stays small -- ticks_ms() is a 32-bit ms counter that wraps every
// ~49.7 days; re-basing on every access keeps that subtraction safely
// within a single wrap regardless of how long it's been since the wall
// clock was last queried or set.
static uint64_t wall_clock_base_ms;
static mp_uint_t wall_clock_base_tick_ms;

static uint64_t wall_clock_now_ms(void) {
    mp_uint_t elapsed_ms = mp_hal_ticks_ms() - wall_clock_base_tick_ms;
    uint64_t now_ms = wall_clock_base_ms + elapsed_ms;
    wall_clock_base_ms = now_ms;
    wall_clock_base_tick_ms = mp_hal_ticks_ms();
    return now_ms;
}

int _gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    uint64_t now_ms = wall_clock_now_ms();
    tv->tv_sec = now_ms / 1000;
    tv->tv_usec = (now_ms % 1000) * 1000;
    return 0;
}

int _settimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    wall_clock_base_ms = (uint64_t)tv->tv_sec * 1000 + tv->tv_usec / 1000;
    wall_clock_base_tick_ms = mp_hal_ticks_ms();
    return 0;
}
#endif // CONFIG_AMEBADPLUS

void mp_hal_delay_us(mp_uint_t us) {
    // these constants are tested for a 240MHz clock
    const uint32_t this_overhead = 5;
    const uint32_t pend_overhead = 150;

    // return if requested delay is less than calling overhead
    if (us < this_overhead) {
        return;
    }
    us -= this_overhead;

    uint64_t t0 = mp_hal_ticks_us();
    for (;;) {
        uint64_t dt = mp_hal_ticks_us() - t0;
        if (dt >= us) {
            return;
        }
        if (dt + pend_overhead < us) {
            // we have enough time to service pending events
            // (don't use MICROPY_EVENT_POLL_HOOK because it also yields)
            mp_handle_pending(true);
        }
    }
}

void mp_hal_delay_ms(mp_uint_t ms) {
    uint64_t us = (uint64_t)ms * 1000ULL;
    uint64_t dt;
    uint64_t t0 = mp_hal_ticks_us();
    for (;;) {
        mp_handle_pending(true);
        MICROPY_PY_SOCKET_EVENTS_HANDLER
        MP_THREAD_GIL_EXIT();
        uint64_t t1 = mp_hal_ticks_us();
        dt = t1 - t0;
        if (dt + portTICK_PERIOD_MS * 1000ULL >= us) {
            // doing a vTaskDelay would take us beyond requested delay time
            taskYIELD();
            MP_THREAD_GIL_ENTER();
            t1 = mp_hal_ticks_us();
            dt = t1 - t0;
            break;
        } else {
            ulTaskNotifyTake(pdFALSE, 1);
            MP_THREAD_GIL_ENTER();
        }
    }
    if (dt < us) {
        // do the remaining delay accurately
        mp_hal_delay_us(us - dt);
    }
}

#if !MICROPY_KBD_EXCEPTION
void mp_hal_set_interrupt_char(int c) {

}
#endif
u32 log_uart_irq(void *Data)
{
	/* To avoid gcc warnings */
	(void) Data;
	u32 loguart_lsr = LOGUART_GetStatus(LOGUART_DEV);

	if (!(loguart_lsr & LOGUART_BIT_DRDY)) {
		return 0;
	}
	bool PullMode = FALSE;
	u32 LogUartIrqEn = LOGUART_GetIMR();
	LOGUART_SetIMR(0);

    int len = LOGUART_GetRxCount();
    for (int i = 0; i < len; i++) {
        char cdata = LOGUART_GetChar(PullMode);
        if (cdata == mp_interrupt_char) {
            mp_sched_keyboard_interrupt();
        } else {
            if (ringbuf_put(&stdin_ringbuf, cdata) < 0) {
                ++uart_rx_overflow;
            }
        }
    }

	LOGUART_SetIMR(LogUartIrqEn);

	return 0;
}

void rtk_loguart_init(void) {
    #if defined(MICROPY_HW_LOGUART_BAUDRATE)
    // ROM brings LOGUART up at 1500000. Drain any boot output still queued at the
    // ROM baud before switching, so the baud change doesn't corrupt in-flight bytes.
    LOGUART_WaitTxComplete();
    LOGUART_SetBaud(LOGUART_DEV, MICROPY_HW_LOGUART_BAUDRATE);
    #endif

    /* Register Log Uart Callback function */
	InterruptRegister((IRQ_FUN) log_uart_irq, UART_LOG_IRQ, (u32)NULL, INT_PRI_LOWEST);
	InterruptEn(UART_LOG_IRQ, INT_PRI_LOWEST);
	// AmebaDplus has two physical cores (KM4/KM0); AmebaGreen2 has one core
	// (KM4) with TrustZone secure/non-secure worlds instead, so the mask
	// bits are named after worlds (AP/NP) rather than cores. The SDK only
	// runs app/example code in whichever world is the WHC WiFi host (see
	// c_ENABLE_EXAMPLE in cmake/common.cmake); with the SDK's default WHC
	// role assignment that's km4tz (AP), so this port targets km4tz here,
	// not km4ns.
	#if defined(CONFIG_AMEBAGREEN2)
	LOGUART_INTCoreConfig(LOGUART_DEV, LOGUART_BIT_INTR_MASK_AP, ENABLE);
	#else
	LOGUART_INTCoreConfig(LOGUART_DEV, LOGUART_BIT_INTR_MASK_KM4, ENABLE);
	LOGUART_INTCoreConfig(LOGUART_DEV, LOGUART_BIT_INTR_MASK_KM0, DISABLE);
	#endif
}

void mp_hal_get_random(size_t n, void *buf) {
    TRNG_get_random_bytes(buf, (u32)n);
}