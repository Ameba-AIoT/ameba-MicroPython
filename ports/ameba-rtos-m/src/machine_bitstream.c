// SPDX-License-Identifier: MIT
// machine.bitstream for ameba-rtos.
//
// Two-tier dispatch, AmebaDplus (RTL8721Dx) only:
//   Path A: LEDC hardware DMA mode      (len % 3 == 0, LED count in range, GDMA channel available)
//   Path C: software SysTick bit-bang   (everything else, incl. no GDMA channel free)
//
// Other SoCs (e.g. AmebaGreen2) always use Path C: their LEDC-equivalent
// block uses a completely different, incompatible API (see the
// CONFIG_AMEBADPLUS guard below) that hasn't been ported yet.
//
// LEDC is a WS2812-shaped hardware block: LEDC_DATA_REG masks to 24 valid
// bits (LEDC_MASK_DATA = 0x00FFFFFF) with no register to change that width.
// It can only accelerate 3-byte-per-unit (RGB) transfers; RGBW (4 bytes/LED)
// and any other non-multiple-of-3 buffer falls back to the original
// software path, unchanged.
//
// There used to be a "Path B" (LEDC hardware, CPU-polled FIFO refill instead
// of DMA) for when no GDMA channel was free. It's removed: LEDC_SetTransferMode()
// unconditionally enables LEDC_BIT_FIFO_CPUREQ_INT_EN for CPU mode, but nothing
// in this driver registers an NVIC handler for LEDC_IRQ or otherwise services
// that interrupt, and on hardware the transfer stalled after the first FIFO
// burst (observed: only a few LEDs' worth of waveform, then a timeout).
// The official raw_ledc_ws2812 example only ever exercises DMA mode too
// (LEDC_MODE is hardcoded to LEDC_DMA_MODE), so CPU mode was never a
// vendor-validated path for this use case. If no GDMA channel is free, we
// now go straight to the software fallback instead of debugging a second
// hardware mode.
//
// Path A never disables global interrupts -- LEDC's hardware state machine
// generates the T0H/T0L/T1H/T1L pulses autonomously; software only supplies
// data via DMA and busy-waits on the raw LEDC_LED_INT_STS_REG completion bit
// (no ISR registration needed for that part).
//
// Path C (software bit-bang) is unchanged from the original implementation:
// timing uses the SysTick counter (CPU clock = 240 MHz).  The RTL8721Dx KM4
// does not implement DWT CYCCNT (DWT_CTRL.NOCYCCNT == 1), so SysTick is the
// only available cycle counter.  GPIO_DR is written directly to reduce each
// toggle to a single read-modify-write on the LSYS APB4 bus.

#include "py/runtime.h"
#include "py/mphal.h"
#include "extmod/modmachine.h"
#include "ameba_soc.h"

#if MICROPY_PY_MACHINE_BITSTREAM

#define NS_CYCLES_OVERHEAD (0)

// LEDC_SetLEDNum() asserts Num <= LEDC_MAX_LED_NUM (1024) -- this is the
// real hardware ceiling for the LEDC path, smaller than LEDC_MAX_DATA_LENGTH
// (8192, which only bounds LEDC_SetTotalLength). Exceeding it must fall
// back to software rather than trip that assert and crash the device.
#define BITSTREAM_LEDC_MAX_LEDS (1024)

// ---------------------------------------------------------------------------
// Path C: original software SysTick bit-bang (unchanged behavior).
// ---------------------------------------------------------------------------
static void bitstream_software_fallback(mp_hal_pin_obj_t pin, uint32_t *timing_ns,
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

    // Explicitly reclaim the pad for GPIO before driving GPIO_DR directly.
    // If Path A (LEDC) ran on this same pin earlier, its Pinmux_Config(pin,
    // PINMUX_FUNCTION_LEDC) call left the pad wired to LEDC; mp_hal_pin_output()
    // won't undo that on its own, since mp_hal_pin_gpio() only runs gpio_init()
    // (which is what calls Pinmux_Config(pin, PINMUX_FUNCTION_GPIO)) once per
    // pin, the first time it's ever used -- it has no way to know the pad's
    // mux function was changed out from under it by a raw SDK call. Without
    // this, GPIO_DR writes below reach a GPIO peripheral that no longer owns
    // the pad, producing no waveform at all.
    Pinmux_Config((u8)pin, PINMUX_FUNCTION_GPIO);

    // Configure pin as output before the atomic section so lazy gpio_init()
    // (which takes an RTOS lock) does not run with IRQs disabled.
    mp_hal_pin_output(pin);

    // Direct GPIO_DR access, bypassing the HAL chain (~400 ns on LSYS APB4).
    // PinName: bit[5] = port (0 = A, 1 = B), bits[4:0] = pin number.
    // GPIO_TypeDef.PORT is declared as an array (PORT[1]) on AmebaDplus but as
    // a plain struct member on AmebaGreen2 -- same field, different C shape.
    uint32_t bit_mask = (1u << (pin & 0x1Fu));
    #if defined(CONFIG_AMEBADPLUS)
    volatile uint32_t *gpio_dr = ((pin >> 5) == 0)
        ? &GPIOA_BASE->PORT[0].GPIO_DR
        : &GPIOB_BASE->PORT[0].GPIO_DR;
    #else
    volatile uint32_t *gpio_dr = ((pin >> 5) == 0)
        ? &GPIOA_BASE->PORT.GPIO_DR
        : &GPIOB_BASE->PORT.GPIO_DR;
    #endif

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

// Path A (LEDC hardware) is AmebaDplus-specific. AmebaGreen2 has a different,
// incompatible LEDC block (ameba_ledc_pro.h -- no overlap at all with the
// LEDC_Init()/LEDC_InitTypeDef/LEDC_DMA_MODE API used below) that would need
// its own from-scratch port, not a drop-in adaptation. Until/unless that's
// done, other SoCs always use the software fallback (Path C).
#if defined(CONFIG_AMEBADPLUS)

// ---------------------------------------------------------------------------
// Shared helper: pack a 3*n_leds-byte buffer into n_leds cache-line-aligned
// 24-in-32-bit words for the LEDC FIFO.  Returns NULL on allocation failure
// (caller must fall back to Path C).  *out_raw / *out_raw_size let the
// caller free the exact block later with m_del.
// ---------------------------------------------------------------------------
static uint32_t *bitstream_ledc_pack(const uint8_t *buf, size_t n_leds,
    uint8_t **out_raw, size_t *out_raw_size) {
    size_t raw_size = n_leds * sizeof(uint32_t) + CACHE_LINE_SIZE;
    uint8_t *raw = m_new(uint8_t, raw_size);
    if (raw == NULL) {
        return NULL;
    }
    uint32_t *staging = (uint32_t *)(((uintptr_t)raw + CACHE_LINE_SIZE - 1)
        & ~(uintptr_t)(CACHE_LINE_SIZE - 1));

    for (size_t i = 0; i < n_leds; ++i) {
        staging[i] = ((uint32_t)buf[i * 3] << 16)
                   | ((uint32_t)buf[i * 3 + 1] << 8)
                   | buf[i * 3 + 2];
    }
    // Match the official raw_ledc_ws2812 example, which uses CleanInvalidate
    // (not Clean-only) on the DMA source buffer before starting the transfer.
    DCache_CleanInvalidate((u32)staging, (u32)(n_leds * sizeof(uint32_t)));

    *out_raw = raw;
    *out_raw_size = raw_size;
    return staging;
}

// ---------------------------------------------------------------------------
// Shared helper: configure LEDC pinmux + timing + counts for this transfer.
//
// Uses the full LEDC_StructInit() + LEDC_Init() flow rather than poking
// individual T0H/T0L registers. This matters because LEDC_Init() also loads
// reset_ns / wait_data_time_ns / wait_time0/1 (which tell the state machine
// when a frame ends) and calls LEDC_INTConfig(LEDC_INT_EXT_EN) at the end.
// Without those, LEDC_BIT_LED_TRANS_FINISH_INT is never raised and the
// completion busy-wait spins forever (observed on hardware as a full hang).
//
// LEDC's counter fields tick every 25 ns (40 MHz IP clock). The struct's
// t0h_ns/etc. fields are misleadingly named -- they hold raw tick counts, so
// timing_ns[] (in nanoseconds, from neopixel) must be divided by 25, exactly
// as the official example does via its NS2VAL() macro.
// ---------------------------------------------------------------------------
#define BITSTREAM_LEDC_NS_PER_TICK (25)

static void bitstream_ledc_configure(mp_hal_pin_obj_t pin, uint32_t *timing_ns,
    size_t n_leds) {
    RCC_PeriphClockCmd(APBPeriph_LEDC, APBPeriph_LEDC_CLOCK, ENABLE);
    Pinmux_Config((u8)pin, PINMUX_FUNCTION_LEDC);

    LEDC_InitTypeDef init;
    LEDC_StructInit(&init);
    init.t0h_ns = timing_ns[0] / BITSTREAM_LEDC_NS_PER_TICK;
    init.t0l_ns = timing_ns[1] / BITSTREAM_LEDC_NS_PER_TICK;
    init.t1h_ns = timing_ns[2] / BITSTREAM_LEDC_NS_PER_TICK;
    init.t1l_ns = timing_ns[3] / BITSTREAM_LEDC_NS_PER_TICK;
    init.led_count = (u32)n_leds;
    init.data_length = (u32)n_leds;
    init.ledc_trans_mode = LEDC_DMA_MODE;
    // LEDC_StructInit() defaults wait_time0_en to ENABLE with a 6us gap
    // inserted between every pair of LED words (LEDC_WAIT_TIME0_CTRL_REG).
    // That is a generic "inter-package" spacing feature of the IP, not part
    // of the WS2812 protocol, and neither the software fallback path nor any
    // other bitstream backend produces it -- disable it so pixels stream
    // back-to-back with no gap, matching the intended bit-exact waveform.
    init.wait_time0_en = DISABLE;
    LEDC_Init(LEDC_DEV, &init);
}

// Upper bound on how long to wait for LEDC_BIT_LED_TRANS_FINISH_INT before
// giving up. A full 1024-LED frame is ~1.3 ms of bit time plus the reset
// window, so 100 ms is generously above any legitimate transfer while still
// guaranteeing the call returns instead of hanging the board if the hardware
// never signals completion (which is exactly the failure mode a missing
// LEDC_Init config caused before this was hardened).
#define BITSTREAM_LEDC_TIMEOUT_US (100000)

// Poll the transfer-finish flag with a timeout. Returns true on completion,
// false on timeout.
static bool bitstream_ledc_wait_done(void) {
    mp_uint_t start = mp_hal_ticks_us();
    while (!(LEDC_GetINT(LEDC_DEV) & LEDC_BIT_LED_TRANS_FINISH_INT)) {
        if (mp_hal_ticks_us() - start > BITSTREAM_LEDC_TIMEOUT_US) {
            return false;
        }
    }
    return true;
}

// Minimal GDMA transfer-complete ISR: just acknowledge the interrupt. The
// actual completion is detected by polling LEDC's TRANS_FINISH flag, but
// GDMA_Init() enables the channel's interrupt and GDMA_ChnlAlloc() registers
// this handler in the NVIC -- a NULL handler here would jump to address 0 and
// fault when the transfer-complete interrupt fires (this was the original hang).
static u8 s_ledc_dma_ch;
static u32 bitstream_ledc_dma_irq(void *param) {
    (void)param;
    GDMA_ClearINT(0, s_ledc_dma_ch);
    return 0;
}

// ---------------------------------------------------------------------------
// Path A: LEDC hardware DMA mode.  Takes a GDMA channel already allocated by
// the caller.  GDMA struct fields (Msize, IsrType) mirror the official
// raw_ledc_ws2812 example -- a zero-filled struct left the channel
// misconfigured and hung GDMA_Init().
// ---------------------------------------------------------------------------
static void bitstream_ledc_dma(uint32_t *staging, size_t n_leds, u8 ch) {
    // Match the example: FIFO trigger level 15 -> 16-word bursts.
    LEDC_SetFIFOLevel(LEDC_DEV, 15);

    GDMA_InitTypeDef gdma = {0};
    gdma.GDMA_Index = 0;
    gdma.GDMA_ChNum = ch;
    gdma.GDMA_DIR = TTFCMemToPeri_PerCtrl;
    gdma.GDMA_DstHandshakeInterface = GDMA_HANDSHAKE_INTERFACE_LEDC_TX;
    gdma.GDMA_IsrType = (BlockType | TransferType | ErrType);
    gdma.GDMA_SrcAddr = (u32)staging;
    gdma.GDMA_DstAddr = (u32)&LEDC_DEV->LEDC_DATA_REG;
    gdma.GDMA_SrcInc = IncType;
    gdma.GDMA_DstInc = NoChange;
    gdma.GDMA_SrcMsize = MsizeSixteen;
    gdma.GDMA_DstMsize = MsizeSixteen;
    gdma.GDMA_SrcDataWidth = TrWidthFourBytes;
    gdma.GDMA_DstDataWidth = TrWidthFourBytes;
    GDMA_Init(0, ch, &gdma);

    // Enable GDMA before LEDC: the transfer is peripheral-controlled
    // handshake, so GDMA sits idle until LEDC's FIFO actually requests a
    // word.  Matches the official raw_ledc_ws2812 example's ordering.
    GDMA_Cmd(0, ch, ENABLE);
    LEDC_Cmd(LEDC_DEV, ENABLE);

    bool ok = bitstream_ledc_wait_done();

    GDMA_Cmd(0, ch, DISABLE);
    GDMA_ChnlFree(0, ch);
    if (!ok) {
        // Reset the peripheral so a timed-out transfer doesn't leave it
        // half-configured for the next call.
        LEDC_SoftReset(LEDC_DEV);
    }
}

#endif // CONFIG_AMEBADPLUS

// ---------------------------------------------------------------------------
// Entry point (upstream extmod contract: synchronous, blocks until sent).
// ---------------------------------------------------------------------------
void machine_bitstream_high_low(mp_hal_pin_obj_t pin, uint32_t *timing_ns,
    const uint8_t *buf, size_t len) {

    if (len == 0) {
        return;
    }

    #if defined(CONFIG_AMEBADPLUS)
    size_t n_leds = len / 3;
    if ((len % 3) == 0 && n_leds > 0 && n_leds <= BITSTREAM_LEDC_MAX_LEDS) {
        // Grab the GDMA channel before packing: no point copying the buffer
        // if there's no channel free and we're about to fall back anyway.
        u8 ch = GDMA_ChnlAlloc(0, (IRQ_FUN)bitstream_ledc_dma_irq, 0, 4);
        if (ch != 0xFF) {
            uint8_t *raw;
            size_t raw_size;
            uint32_t *staging = bitstream_ledc_pack(buf, n_leds, &raw, &raw_size);
            if (staging != NULL) {
                s_ledc_dma_ch = ch;
                bitstream_ledc_configure(pin, timing_ns, n_leds);
                bitstream_ledc_dma(staging, n_leds, ch);
                m_del(uint8_t, raw, raw_size);
                return;
            }
            // m_new failed -- free the channel and fall through to software.
            GDMA_ChnlFree(0, ch);
        }
        // No GDMA channel free -- fall through to the software path below.
    }
    #endif // CONFIG_AMEBADPLUS

    bitstream_software_fallback(pin, timing_ns, buf, len);
}

#endif // MICROPY_PY_MACHINE_BITSTREAM
