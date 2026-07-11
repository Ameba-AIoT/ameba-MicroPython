// SPDX-License-Identifier: MIT
// machine.I2S port implementation for ameba-rtos.
//
// INCLUDEFILE — included by extmod/machine_i2s.c via
// MICROPY_PY_MACHINE_I2S_INCLUDEFILE.
//
// Hardware: SPORT (Serial Port) controller, driven by GDMA.
//
// Sample data moves between the SPORT FIFO and a DMA buffer over a GDMA
// channel — the SDK's intended (and only validated) mechanism for the SPORT.
// An earlier revision fed the TX FIFO from a SPORT FIFO-empty interrupt with
// the controller in DSP handshake mode (DSP_CTL_MODE=1); that path proved
// unreliable with real data (the ISR stopped draining the ring buffer, hanging
// a blocking write()) and has no SDK precedent.  GDMA was previously avoided on
// a belief that it interfered with LOGUART RX, but LOGUART on this port is not
// GDMA-driven (REPL input is interrupt + ring buffer), so that concern does not
// apply.
//
// The DMA buffer is a ping-pong pair of halves driven by a 2-node circular
// GDMA linked list (LLP / block chaining, AUDIO_SP_LLPTxGDMA_Init): the
// hardware transfers half 0, then automatically follows the chain to half 1,
// then back to half 0, forever — the ISR never re-arms the channel.  Each
// block-complete interrupt only acknowledges the IRQ and refills the half that
// just finished from the ring buffer (TX) / drains it into the ring buffer
// (RX), so that half is ready by the time the chain loops back to it.  An
// earlier revision used single-block GDMA that the ISR stopped and restarted
// per half; under load (a blocking write() feeding real data concurrently) a
// re-arm could be lost and the channel silently stopped draining after a few
// blocks, hanging the write().  The circular chain has no such restart race.
// The ring buffer couples this to the extmod framework's blocking and
// non-blocking copy helpers exactly as the other I2S ports do.
//
// Two I2S peripherals (SPORT0/SPORT1) on AmebaDplus, one (SPORT0) on other
// SoCs (see the CONFIG_AMEBADPLUS guards below).  Supports TX/RX, 16/32 bit,
// MONO/STEREO, 8k-96k sample rates.

#include <stdlib.h>
#include <string.h>

#include "ameba_sport.h"
#include "ameba_soc.h"

// Forward declarations of SDK functions not in the public header.
extern void RCC_PeriphClockSource_SPORT(AUDIO_SPORT_TypeDef *SPx, u32 source);

// AmebaDplus has two SPORT peripherals (SPORT0/SPORT1); other SoCs (e.g.
// AmebaGreen2) have only SPORT0, see the CONFIG_AMEBADPLUS guards below.
#if defined(CONFIG_AMEBADPLUS)
#define I2S_ID_COUNT    (2)
#else
#define I2S_ID_COUNT    (1)
#endif

// DMA buffer = ring buffer / 4
#define I2S_DMA_BUF_DIV  (4)

// Non-blocking copy chunk size (bytes per ISR invocation).
#define SIZEOF_NON_BLOCKING_COPY_IN_BYTES  (256)

// I2S frame map: maps byte positions between 8-byte I2S frames and
// application buffers.  Required by the extmod INCLUDEFILE contract
// (fill_appbuf_from_ringbuf / copy_appbuf_to_ringbuf reference both).
// Index 0..3 = (16-bit mono, 16-bit stereo, 32-bit mono, 32-bit stereo).
// -1 means discard (RX) or pad with 0 (TX).
static const int8_t i2s_frame_map[4][8] = {
    {0, 1, -1, -1, -1, -1, -1, -1},
    {0, 1, -1, -1, 2, 3, -1, -1},
    {0, 1, 2, 3, -1, -1, -1, -1},
    {0, 1, 2, 3, 4, 5, 6, 7},
};

static uint8_t get_frame_mapping_index(uint32_t bits, uint32_t format) {
    if (bits == 16 && format == MONO) return 0;
    if (bits == 16 && format == STEREO) return 1;
    if (bits == 32 && format == MONO) return 2;
    return 3; // 32-bit STEREO
}

// Forward declarations for extmod.
typedef struct _machine_i2s_obj_t machine_i2s_obj_t;
static void start_transfer(machine_i2s_obj_t *self);
static void mp_machine_i2s_deinit(machine_i2s_obj_t *self);

struct _machine_i2s_obj_t {
    mp_obj_base_t base;

    uint8_t i2s_id;

    mp_hal_pin_obj_t sck;
    mp_hal_pin_obj_t ws;
    mp_hal_pin_obj_t sd;
    #if MICROPY_PY_MACHINE_I2S_MCK
    mp_hal_pin_obj_t mck;
    #endif

    uint32_t mode;
    uint32_t bits;
    uint32_t format;
    uint32_t rate;
    uint32_t ibuf;
    uint32_t io_mode;

    ring_buf_t ring_buffer;
    uint8_t *ring_buffer_ptr;

    // DMA ping-pong buffer.  dma_buffer is a 32-byte-aligned view into
    // dma_buffer_raw (the malloc'd block, kept for free()).  It is split into
    // two equal halves; GDMA transfers one half while the CPU prepares the
    // other.  dma_buffer_size is the size of the whole (two-half) buffer.
    uint8_t *dma_buffer_raw;
    uint8_t *dma_buffer;
    uint32_t dma_buffer_size;
    // Written by the thread (start_transfer / deinit) and read by the GDMA ISR.
    // Marked volatile so the access cannot be optimised away on its own; this is
    // independent of the file-wide -O0 workaround (see src/CMakeLists.txt), which
    // exists only to keep the *upstream* ring_buf_t head/tail re-read in the
    // blocking spin loop — those fields live in upstream code we may not annotate.
    volatile bool dma_active;

    // GDMA channel state.  gdma_struct is filled in by AUDIO_SP_LLPTxGDMA_Init /
    // AUDIO_SP_LLPRxGDMA_Init (it holds GDMA_Index / GDMA_ChNum used by ClearINT,
    // abort and channel free).  dma_half is the index (0/1) of the half whose
    // block-complete interrupt is expected next (i.e. the half currently being
    // transferred by the GDMA).
    //
    // gdma_lli is a 2-node circular linked list (LLP / block chaining): node 0
    // points at the first half, node 1 at the second, and each node's pNextLli
    // points at the other, so the GDMA hardware cycles half0 -> half1 -> half0
    // forever WITHOUT the ISR ever re-arming the channel.  This removes the
    // software stop-and-restart race that made the single-block path silently
    // stop draining after a few blocks under load.  It must be 32-byte aligned
    // (one cache line per node); gdma_lli is an aligned view into gdma_lli_raw
    // (kept for free()).
    GDMA_InitTypeDef gdma_struct;
    bool gdma_initted;
    // Toggled by the GDMA ISR and seeded by start_transfer (thread); volatile for
    // the same reason as dma_active above.
    volatile uint32_t dma_half;
    struct GDMA_CH_LLI *gdma_lli;
    void *gdma_lli_raw;

    non_blocking_descriptor_t non_blocking_descriptor;
    mp_obj_t callback_for_non_blocking;
};

// I2S mode constants.  Must match SP_DIR_TX (1) and SP_DIR_RX (2)
// so that self->mode can be passed directly to AUDIO_SP_Init.
#define MICROPY_PY_MACHINE_I2S_CONSTANT_TX               (1)
#define MICROPY_PY_MACHINE_I2S_CONSTANT_RX               (2)

// ---------------------------------------------------------------------------
// GDMA ping-pong helpers and completion callbacks.
// ---------------------------------------------------------------------------

// TX: refill one half of the DMA buffer from the ring buffer.  On underrun
// (not a full half available) the half is zero-filled so the I2S bus emits
// silence instead of stale samples.  The half is flushed to RAM so the GDMA
// reads the fresh data.
static void i2s_tx_feed_half(machine_i2s_obj_t *self, uint32_t which) {
    uint32_t half = self->dma_buffer_size / 2;
    uint8_t *p = self->dma_buffer + which * half;

    if (ringbuf_available_data(&self->ring_buffer) >= half) {
        for (uint32_t i = 0; i < half; i++) {
            ringbuf_pop(&self->ring_buffer, &p[i]);
        }
    } else {
        memset(p, 0, half);
    }
    DCache_CleanInvalidate((u32)p, half);
}

// RX: drain one half of the DMA buffer into the ring buffer.  The half is
// invalidated first so the CPU sees the data the GDMA just wrote.  If the ring
// buffer is full the data is dropped (overrun).
static void i2s_rx_drain_half(machine_i2s_obj_t *self, uint32_t which) {
    uint32_t half = self->dma_buffer_size / 2;
    uint8_t *p = self->dma_buffer + which * half;

    DCache_Invalidate((u32)p, half);
    if (ringbuf_available_space(&self->ring_buffer) >= half) {
        for (uint32_t i = 0; i < half; i++) {
            ringbuf_push(&self->ring_buffer, p[i]);
        }
    }
}

// TX GDMA block-complete callback.  With the circular LLP chain the hardware
// has ALREADY advanced to the other half on its own — the ISR only acknowledges
// the interrupt and refills the half that just finished, so it is ready when the
// chain loops back to it.  No channel re-arming, hence no restart race.
static u32 i2s_tx_gdma_done(void *arg) {
    machine_i2s_obj_t *self = (machine_i2s_obj_t *)arg;

    // GDMA_ClearINT clears and returns the pending interrupt-type bitmask.  The
    // channel is armed for Block|Transfer|Err (see AUDIO_SP_LLPTXGDMA_Init); only
    // a block/transfer completion means the hardware has advanced to the other
    // half.  An error interrupt must NOT toggle dma_half — doing so would desync
    // the ping-pong from the GDMA's real position.  Mirrors the SDK ISR idiom.
    uint32_t int_status = GDMA_ClearINT(self->gdma_struct.GDMA_Index, self->gdma_struct.GDMA_ChNum);
    if (!(int_status & (BlockType | TransferType))) {
        return 0;
    }

    uint32_t completed = self->dma_half;
    self->dma_half = completed ^ 1;

    i2s_tx_feed_half(self, completed);

    // Non-blocking mode: the pops above freed ring space, so hand off to the
    // framework helper to copy the next chunk of the user buffer in.  The
    // helper clears copy_in_progress and schedules the callback itself.
    if (self->io_mode == NON_BLOCKING &&
        self->non_blocking_descriptor.copy_in_progress) {
        copy_appbuf_to_ringbuf_non_blocking(self);
    }
    return 0;
}

// RX GDMA block-complete callback.  Mirror of TX: the hardware has advanced to
// the other half already; drain the just-filled half into the ring buffer.
static u32 i2s_rx_gdma_done(void *arg) {
    machine_i2s_obj_t *self = (machine_i2s_obj_t *)arg;

    // See i2s_tx_gdma_done: only advance the ping-pong on a block/transfer
    // completion, never on an error interrupt.
    uint32_t int_status = GDMA_ClearINT(self->gdma_struct.GDMA_Index, self->gdma_struct.GDMA_ChNum);
    if (!(int_status & (BlockType | TransferType))) {
        return 0;
    }

    uint32_t completed = self->dma_half;
    self->dma_half = completed ^ 1;

    i2s_rx_drain_half(self, completed);

    if (self->io_mode == NON_BLOCKING &&
        self->non_blocking_descriptor.copy_in_progress) {
        fill_appbuf_from_ringbuf_non_blocking(self);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// make_new_instance
// ---------------------------------------------------------------------------
static machine_i2s_obj_t *mp_machine_i2s_make_new_instance(mp_int_t i2s_id) {
    if (i2s_id < 0 || i2s_id >= I2S_ID_COUNT) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("I2S(%d) doesn't exist"), (int)i2s_id);
    }

    // Keep exactly one object per SPORT, rooted in MP_STATE_PORT so the GC
    // never collects an instance whose ISR is still registered.  If this
    // SPORT is already in use, reuse the existing object and tear its
    // hardware down first — re-running init on a live SPORT (re-registering
    // the NVIC IRQ, resetting the controller mid-handshake) hangs the chip.
    machine_i2s_obj_t *self;
    if (MP_STATE_PORT(machine_i2s_obj)[i2s_id] == NULL) {
        self = mp_obj_malloc(machine_i2s_obj_t, &machine_i2s_type);
        MP_STATE_PORT(machine_i2s_obj)[i2s_id] = self;
        self->i2s_id = (uint8_t)i2s_id;
    } else {
        self = MP_STATE_PORT(machine_i2s_obj)[i2s_id];
        mp_machine_i2s_deinit(self);
    }

    self->dma_active = false;
    self->dma_buffer = NULL;
    self->dma_buffer_raw = NULL;
    self->ring_buffer_ptr = NULL;
    self->gdma_initted = false;
    self->dma_half = 0;
    self->gdma_lli = NULL;
    self->gdma_lli_raw = NULL;
    // Reset the I/O mode on the reuse path too: a fresh mp_obj_malloc is
    // zeroed, but reconstructing an existing instance must not inherit the
    // previous session's NON_BLOCKING mode or its (now stale) Python callback,
    // otherwise the new object would silently behave as non-blocking and keep
    // the old callback object reachable.
    self->io_mode = BLOCKING;
    self->callback_for_non_blocking = MP_OBJ_NULL;
    self->non_blocking_descriptor.copy_in_progress = false;
    return self;
}

// ---------------------------------------------------------------------------
// init_helper
// ---------------------------------------------------------------------------
static void mp_machine_i2s_init_helper(machine_i2s_obj_t *self, mp_arg_val_t *args) {
    // Resolve and validate the pin arguments (string/int/Pin); raises
    // ValueError for an unknown pin instead of dereferencing garbage.
    self->sck    = mp_hal_pin_resolve(args[ARG_sck].u_obj);
    self->ws     = mp_hal_pin_resolve(args[ARG_ws].u_obj);
    self->sd     = mp_hal_pin_resolve(args[ARG_sd].u_obj);
    self->mode   = args[ARG_mode].u_int;
    self->bits   = args[ARG_bits].u_int;
    self->format = args[ARG_format].u_int;
    self->rate   = args[ARG_rate].u_int;
    self->ibuf   = args[ARG_ibuf].u_int;

    // Validate mode/bits/format BEFORE touching any hardware: mode is passed
    // straight to AUDIO_SP_Init as the direction, and an out-of-range value
    // would slip through the SDK's assert (compiled out in release builds) and
    // configure the SPORT with a bogus direction.
    if (self->mode != MICROPY_PY_MACHINE_I2S_CONSTANT_TX &&
        self->mode != MICROPY_PY_MACHINE_I2S_CONSTANT_RX) {
        mp_raise_ValueError(MP_ERROR_TEXT("mode must be TX or RX"));
    }
    if (self->bits != 16 && self->bits != 32) {
        mp_raise_ValueError(MP_ERROR_TEXT("bits must be 16 or 32"));
    }
    if (self->format != MONO && self->format != STEREO) {
        mp_raise_ValueError(MP_ERROR_TEXT("format must be MONO or STEREO"));
    }
    if (self->ibuf < 256) {
        mp_raise_ValueError(MP_ERROR_TEXT("ibuf too small"));
    }

    bool is_tx = (self->mode == MICROPY_PY_MACHINE_I2S_CONSTANT_TX);

    // TX uses the user-requested mono/stereo.  RX must always run in STEREO
    // mode so the SPORT generates 2 slots per I2S frame, giving 8 DMA bytes
    // per frame.  fill_appbuf_from_ringbuf() always consumes exactly 8 ring
    // buffer bytes per iteration (I2S_RX_FRAME_SIZE_IN_BYTES); if MONO were
    // passed through, the SPORT would generate only 4 bytes/frame, the ring
    // buffer would fill at half speed, and readinto() would take 2× longer.
    // The frame_map discards the R-channel bytes for MONO configurations.
    uint32_t sp_mono = (is_tx && self->format == MONO) ? SP_CH_MONO : SP_CH_STEREO;

    // TX word length tracks the requested sample size (verified on a logic
    // analyzer: bits=16 with WL_16 gives 512 kHz BCLK and clean L/R
    // separation; WL_32 would double the clock and merge two 16-bit samples
    // into one slot).  RX must always use WL_32 so the SPORT DMA captures 4
    // bytes per slot (the full 32-bit container).  WL_16 makes the hardware
    // write only 2 bytes per slot; fill_appbuf_from_ringbuf() then waits 2×
    // longer for data and the frame_map -1 discard entries consume real sample
    // bytes from the next slot instead of padding from the current one.
    uint32_t sp_wl = (is_tx && self->bits == 16) ? SP_TXWL_16 : SP_TXWL_32;

    // Channel length (slot width in bit-clocks) is direction-specific:
    //   TX: match the word length so the BCLK rate is correct for the sample size.
    //   RX: always 32-bit channel so the BCLK is 2×32×SR and the DMA slot is
    //       4 bytes wide, matching the 32-bit word length forced above.
    uint32_t sp_cl;
    if (is_tx) {
        sp_cl = (self->bits == 16) ? SP_TXCL_16 : SP_TXCL_32;
    } else {
        sp_cl = SP_RXCL_32;
    }

    uint32_t sp_dir = self->mode;

    u32 sport_dev = self->i2s_id;

    // Enable the SPORT and AC (audio codec) peripheral clocks BEFORE touching
    // any SPORT register.  On AmebaDplus the SPORT sits in the audio-codec
    // clock domain, so without the AC clock the SPORT register block is gated
    // and any access (such as AUDIO_SP_Reset below) raises a bus fault.  On a
    // cold boot these clocks happen to already be on, but after a deinit +
    // soft reset they are gated again — which made a second I2S construct
    // crash in AUDIO_SP_Reset.
    #if defined(CONFIG_AMEBADPLUS)
    RCC_PeriphClockCmd((sport_dev == 0) ? APBPeriph_SPORT0 : APBPeriph_SPORT1,
                       (sport_dev == 0) ? APBPeriph_SPORT0_CLOCK : APBPeriph_SPORT1_CLOCK,
                       ENABLE);
    #else
    RCC_PeriphClockCmd(APBPeriph_SPORT, APBPeriph_SPORT_CLOCK, ENABLE);
    #endif
    RCC_PeriphClockCmd(APBPeriph_AC, APBPeriph_AC_CLOCK, ENABLE);

    // Enable the audio-codec digital IP and its I2S0 sub-block.  The SPORT
    // register block lives behind this master enable: on a cold boot it is
    // left on by the bootrom, but a deinit + soft reset clears it, after
    // which the *next* SPORT register access (e.g. AUDIO_SP_Reset) bus-faults
    // and reads back 0xdeadbeef.  Re-asserting it here makes reconstruction
    // (and every construct past the first) safe.
    AUDIO_CODEC_SetAudioIP(ENABLE);
    AUDIO_CODEC_SetI2SIP(I2S0, ENABLE);

    // Set SPORT clock source to 40MHz XTAL (no PLL needed).
    #if defined(CONFIG_AMEBADPLUS)
    RCC_PeriphClockSource_SPORT(sport_dev == 0 ? AUDIO_SPORT0_DEV : AUDIO_SPORT1_DEV, CKSL_I2S_XTAL40M);
    #else
    RCC_PeriphClockSource_SPORT(AUDIO_SPORT0_DEV, CKSL_I2S_XTAL40M);
    #endif

    // Reset SPORT peripheral before configuring.
    AUDIO_SP_Reset(sport_dev);

    // Route the SPORT bit-clock, word-select and data signals to the
    // user-supplied GPIO pads.  AmebaDplus uses a per-pin pinmux crossbar
    // (Pinmux_Config(PinName, PinFunc)), so any GPIO can be muxed to the
    // I2S function IDs from ameba_pinmux.h.  BCLK/WS are direction-agnostic.
    //
    // The data pad needs care: the SPORT has four data lanes DOUT0..3 / DIN0..3,
    // and the pad crossbar (REG_I2S_CTRL via AUDIO_SP_SetPinMux) pairs each DIO
    // pad lane with ONE fixed serializer lane — DIO0<->DOUT3/DIN0,
    // DIO3<->DOUT0/DIN3.  In standard (non-multi-IO) mode the serializer drives
    // its single stream on DOUT0 and samples it on DIN0.  DOUT0 is reachable
    // only through the DIO3 pad lane, while DIN0 is reachable only through the
    // DIO0 pad lane — so the data pad's pinmux function and the crossbar
    // selection BOTH depend on direction:
    //   TX: pad -> DIO3 function, crossbar DIO3 <- DOUT0  (drive out)
    //   RX: pad -> DIO0 function, crossbar DIO0 <- DIN0   (sample in)
    // A previous version muxed the data pad to DIO0 + DOUT3 for TX; that routed
    // the idle DOUT3 lane to the pad, so the data pin read a static 0 V on a
    // multimeter even though the serializer (verified via internal loopback)
    // was emitting the correct bytes.
    #if defined(CONFIG_AMEBADPLUS)
    uint32_t pf_bclk = (sport_dev == 0) ? PINMUX_FUNCTION_I2S0_BCLK : PINMUX_FUNCTION_I2S1_BCLK;
    uint32_t pf_ws   = (sport_dev == 0) ? PINMUX_FUNCTION_I2S0_WS   : PINMUX_FUNCTION_I2S1_WS;
    uint32_t pf_data;
    if (is_tx) {
        pf_data = (sport_dev == 0) ? PINMUX_FUNCTION_I2S0_DIO3 : PINMUX_FUNCTION_I2S1_DIO3;
    } else {
        pf_data = (sport_dev == 0) ? PINMUX_FUNCTION_I2S0_DIO0 : PINMUX_FUNCTION_I2S1_DIO0;
    }
    #else
    uint32_t pf_bclk = PINMUX_FUNCTION_I2S0_BCLK;
    uint32_t pf_ws   = PINMUX_FUNCTION_I2S0_WS;
    uint32_t pf_data = is_tx ? PINMUX_FUNCTION_I2S0_DIO3 : PINMUX_FUNCTION_I2S0_DIO0;
    #endif
    Pinmux_Config((u8)self->sck, pf_bclk);
    Pinmux_Config((u8)self->ws,  pf_ws);
    Pinmux_Config((u8)self->sd,  pf_data);

    // Set the data-pin direction in the crossbar (REG_I2S_CTRL).  Its reset
    // value is 0 = DIN (input), so a TX instance must select the DOUT lane that
    // pairs with the DIO3 pad above (DOUT0), otherwise the pad stays an input
    // and nothing is driven out.  RX selects DIN0, which pairs with DIO0.
    AUDIO_SP_SetPinMux(sport_dev, is_tx ? DOUT0_FUNC : DIN0_FUNC);

    // Select the codec I2S_0 block's data source.  On AmebaDplus the SPORT does
    // not reach the I2S pads directly — it goes through the audio-codec "I2S_0"
    // digital block, whose master-source mux
    // (CODEC_I2S_0_CONTROL.I2S_0_MASTER_SEL, set via AUDIO_CODEC_SetI2SSRC)
    // chooses between the internal SPORT path (the codec's own ADC/DAC, which is
    // the reset default) and the external I2S pads.  TX (the codec "DAC path")
    // works on the internal-SPORT default because the SPORT drives the DOUT pad
    // through that block.  But RX (the "ADC path") on the internal-SPORT default
    // samples the codec's internal ADC — powered down here — so the external DIN
    // pad is never sampled and every RX word reads a constant 0 (confirmed on
    // hardware: a DIN pad tied to 3V3 still read 0x00, where a real external
    // sample path would read 0xFF).  Point the mux at the external pads for RX;
    // keep it on the internal SPORT for TX so the verified output path is
    // unchanged and so a TX instance that reuses the SPORT after an RX instance
    // restores the bit (AUDIO_SP_Reset does not clear CODEC_I2S_0_CONTROL).
    AUDIO_CODEC_SetI2SSRC(I2S0, is_tx ? INTERNAL_SPORT : EXTERNAL_I2S);

    // Allocate the DMA ping-pong buffer.  Size = ibuf/4, rounded to a multiple
    // of 64 so each of the two halves is a multiple of 32 (the D-cache line
    // size) — this keeps a half's cache maintenance from touching the other
    // half.  Over-allocate by 32 bytes and align the usable base to 32 bytes
    // for the same reason; dma_buffer_raw is kept for free().
    self->dma_buffer_size = (self->ibuf / I2S_DMA_BUF_DIV) & ~(uint32_t)63;
    if (self->dma_buffer_size < 128) self->dma_buffer_size = 128;
    self->dma_buffer_raw = (uint8_t *)malloc(self->dma_buffer_size + 32);
    if (!self->dma_buffer_raw) {
        mp_raise_OSError(MP_ENOMEM);
    }
    self->dma_buffer = (uint8_t *)(((uintptr_t)self->dma_buffer_raw + 31) & ~(uintptr_t)31);

    // Allocate the 2-node GDMA linked list, 32-byte aligned (one cache line per
    // node) so the GDMA reads coherent descriptors.
    self->gdma_lli_raw = malloc(2 * sizeof(struct GDMA_CH_LLI) + 32);
    if (!self->gdma_lli_raw) {
        free(self->dma_buffer_raw);
        self->dma_buffer_raw = NULL;
        self->dma_buffer = NULL;
        mp_raise_OSError(MP_ENOMEM);
    }
    self->gdma_lli = (struct GDMA_CH_LLI *)(((uintptr_t)self->gdma_lli_raw + 31) & ~(uintptr_t)31);

    // Allocate ring buffer.
    self->ring_buffer_ptr = (uint8_t *)malloc(self->ibuf);
    if (!self->ring_buffer_ptr) {
        free(self->dma_buffer_raw);
        self->dma_buffer_raw = NULL;
        self->dma_buffer = NULL;
        free(self->gdma_lli_raw);
        self->gdma_lli_raw = NULL;
        self->gdma_lli = NULL;
        mp_raise_OSError(MP_ENOMEM);
    }
    ringbuf_init(&self->ring_buffer, self->ring_buffer_ptr, self->ibuf);

    // Configure and initialize SPORT using StructInit for proper defaults,
    // then override with our parameters.
    SP_InitTypeDef sp_init;
    AUDIO_SP_StructInit(&sp_init);
    sp_init.SP_SelWordLen = sp_wl;
    sp_init.SP_SelChLen = sp_cl;
    sp_init.SP_SR = self->rate;
    sp_init.SP_SelI2SMonoStereo = sp_mono;

    AUDIO_SP_Init(sport_dev, sp_dir, &sp_init);

    // Disable MCLK output — the SPORT's bit clock and frame sync do
    // not depend on MCLK, but it could interfere with other peripherals.
    AUDIO_SP_SetMclk(sport_dev, DISABLE);

    // Start the SPORT transfer immediately (not deferred to irq_update).
    // The extmod machine_i2s.c framework only calls mp_machine_i2s_irq_update()
    // from the .irq() method (non-blocking callback setup).  In blocking mode,
    // .write() and .readinto() copy to/from the ring buffer without ever
    // calling irq_update, so the SPORT must already be running.
    start_transfer(self);
}

// ---------------------------------------------------------------------------
// start_transfer — deferred from init
// ---------------------------------------------------------------------------
static void start_transfer(machine_i2s_obj_t *self) {
    u32 sport_dev = self->i2s_id;
    bool is_tx = (self->mode == MICROPY_PY_MACHINE_I2S_CONSTANT_TX);
    uint32_t half = self->dma_buffer_size / 2;

    // GDMA handshake mode: AUDIO_SP_DmaCmd(ENABLE) clears DSP_CTL_MODE so the
    // SPORT requests data over the GDMA handshake interface instead of expecting
    // the CPU to feed the FIFO.
    AUDIO_SP_DmaCmd(sport_dev, ENABLE);
    self->dma_half = 0;

    // Build the 2-node circular linked list.  The LLP init helpers fill in the
    // FIFO-side address (Darx for TX, Sarx for RX), the block size and the
    // control words; the caller owns the memory-side address of each node and
    // the pNextLli ring.  Wiring node 0 <-> node 1 makes the chain loop forever.
    self->gdma_lli[0].pNextLli = &self->gdma_lli[1];
    self->gdma_lli[1].pNextLli = &self->gdma_lli[0];

    if (is_tx) {
        // Memory-side (source) address of each block = the two halves.
        self->gdma_lli[0].LliEle.Sarx = (u32)(self->dma_buffer);
        self->gdma_lli[1].LliEle.Sarx = (u32)(self->dma_buffer + half);

        // Prepare both halves (ring is empty at first start, so both are
        // silence) and kick the TX engine, then start the circular GDMA.
        // AUDIO_SP_LLPTXGDMA_Init allocates a channel, registers
        // i2s_tx_gdma_done as the per-block handler and enables the chain.
        i2s_tx_feed_half(self, 0);
        i2s_tx_feed_half(self, 1);

        AUDIO_SP_TXStart(sport_dev, ENABLE);

        AUDIO_SP_LLPTXGDMA_Init(sport_dev, GDMA_INT, &self->gdma_struct, (void *)self,
            (IRQ_FUN)i2s_tx_gdma_done, half, 2, self->gdma_lli);
        self->gdma_initted = true;
    } else {
        // Memory-side (destination) address of each block = the two halves.
        self->gdma_lli[0].LliEle.Darx = (u32)(self->dma_buffer);
        self->gdma_lli[1].LliEle.Darx = (u32)(self->dma_buffer + half);

        // RX: start the engine and the circular GDMA; i2s_rx_gdma_done drains
        // each filled half into the ring buffer.
        AUDIO_SP_RXStart(sport_dev, ENABLE);

        AUDIO_SP_LLPRXGDMA_Init(sport_dev, GDMA_INT, &self->gdma_struct, (void *)self,
            (IRQ_FUN)i2s_rx_gdma_done, half, 2, self->gdma_lli);
        self->gdma_initted = true;
    }

    self->dma_active = true;
}

// ---------------------------------------------------------------------------
// deinit
// ---------------------------------------------------------------------------
static void mp_machine_i2s_deinit(machine_i2s_obj_t *self) {
    if (self->dma_active) {
        // Stop the GDMA channel first so it cannot fire a block callback while
        // we tear the SPORT down.  The circular LLP chain never "completes" on
        // its own, so abort it (force-stop mid-block), then free the channel
        // allocated by AUDIO_SP_LLPTxGDMA_Init / LLPRxGDMA_Init.
        if (self->gdma_initted) {
            GDMA_Abort(self->gdma_struct.GDMA_Index, self->gdma_struct.GDMA_ChNum);
            GDMA_ClearINT(self->gdma_struct.GDMA_Index, self->gdma_struct.GDMA_ChNum);
            GDMA_ChnlFree(self->gdma_struct.GDMA_Index, self->gdma_struct.GDMA_ChNum);
            self->gdma_initted = false;
        }

        // Stop SPORT.
        AUDIO_SP_TXStart(self->i2s_id, DISABLE);
        AUDIO_SP_RXStart(self->i2s_id, DISABLE);
        AUDIO_SP_DmaCmd(self->i2s_id, DISABLE);

        // KNOWN ISSUE (2026-07-11): constructing an RX instance immediately
        // after deinit()ing a TX instance on the same SPORT hangs readinto()
        // forever (no timeout in the shared extmod ring-buffer wait loop, so
        // it requires a hardware reset). Reordering this teardown to stop
        // SPORT/DMA before freeing the GDMA channel (matching the SDK's own
        // audio_deinit() example order) did not fix it, nor did unconditionally
        // calling AUDIO_SP_TXStart(ENABLE) before AUDIO_SP_RXStart(ENABLE) in
        // start_transfer() (RX-only from a fresh boot works fine either way --
        // only the TX-then-RX transition on one SPORT instance hangs). Root
        // cause not found; see findings.md.

        // De-register and reset SPORT.
        AUDIO_SP_Unregister(self->i2s_id, SP_DIR_TX);
        AUDIO_SP_Unregister(self->i2s_id, SP_DIR_RX);
        AUDIO_SP_Reset(self->i2s_id);

        // Restore the I2S pins to plain GPIO so they can be reused by other
        // peripherals after deinit, mirroring the machine.SPI teardown.
        Pinmux_Config((u8)self->sck, PINMUX_FUNCTION_GPIO);
        Pinmux_Config((u8)self->ws,  PINMUX_FUNCTION_GPIO);
        Pinmux_Config((u8)self->sd,  PINMUX_FUNCTION_GPIO);

        self->dma_active = false;
    }

    if (self->ring_buffer_ptr) {
        free(self->ring_buffer_ptr);
        self->ring_buffer_ptr = NULL;
    }
    if (self->dma_buffer_raw) {
        free(self->dma_buffer_raw);
        self->dma_buffer_raw = NULL;
        self->dma_buffer = NULL;
    }
    if (self->gdma_lli_raw) {
        free(self->gdma_lli_raw);
        self->gdma_lli_raw = NULL;
        self->gdma_lli = NULL;
    }
}

// ---------------------------------------------------------------------------
// deinit_all — called from the port soft-reset path (mp_main.c)
// ---------------------------------------------------------------------------
// The I2S instance objects are heap-allocated and rooted in MP_STATE_PORT.
// MP_STATE_PORT survives a soft reset, but the GC heap it points into is swept
// and re-initialised — so without clearing these pointers the next session's
// make_new would take the "reuse" branch and call deinit() on a dangling
// pointer, dereferencing freed memory (a MemManage fault in AUDIO_SP_TXStart).
// Tear down any live SPORT hardware while the heap is still valid, then drop
// the pointers so the next session constructs fresh instances.
void machine_i2s_deinit_all(void) {
    for (int i = 0; i < I2S_ID_COUNT; i++) {
        machine_i2s_obj_t *self = MP_STATE_PORT(machine_i2s_obj)[i];
        if (self != NULL) {
            mp_machine_i2s_deinit(self);
            MP_STATE_PORT(machine_i2s_obj)[i] = NULL;
        }
    }
}

// ---------------------------------------------------------------------------
// irq_update — starts the deferred transfer on first call
// ---------------------------------------------------------------------------
static void mp_machine_i2s_irq_update(machine_i2s_obj_t *self) {
    if (!self->dma_active) {
        start_transfer(self);
    }
}

// One live instance per SPORT, GC-rooted so an active ISR's self pointer
// stays valid even if all Python references are dropped.
MP_REGISTER_ROOT_POINTER(struct _machine_i2s_obj_t *machine_i2s_obj[I2S_ID_COUNT]);
