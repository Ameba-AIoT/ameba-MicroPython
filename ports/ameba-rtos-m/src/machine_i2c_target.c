/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: MIT
 */

// This file is never compiled standalone; it is included directly from
// extmod/machine_i2c_target.c via MICROPY_PY_MACHINE_I2C_TARGET_INCLUDEFILE.

#include "py/mphal.h"
#include "shared/runtime/mpirq.h"
#include "i2c_api.h"
#include "ameba_i2c.h"
#include "ameba_vector.h"

// ---------------------------------------------------------------------------
// Default pins — overridden per-board via mpconfigboard.h.
// ---------------------------------------------------------------------------
#ifndef MICROPY_HW_I2C0_SCL
#define MICROPY_HW_I2C0_SCL (PA_26)
#endif
#ifndef MICROPY_HW_I2C0_SDA
#define MICROPY_HW_I2C0_SDA (PA_27)
#endif
#ifndef MICROPY_HW_I2C1_SCL
#define MICROPY_HW_I2C1_SCL (PA_28)
#endif
#ifndef MICROPY_HW_I2C1_SDA
#define MICROPY_HW_I2C1_SDA (PA_29)
#endif

static const PinName i2c_target_default_scl[MICROPY_PY_MACHINE_I2C_TARGET_MAX] = {
    MICROPY_HW_I2C0_SCL, MICROPY_HW_I2C1_SCL,
};
static const PinName i2c_target_default_sda[MICROPY_PY_MACHINE_I2C_TARGET_MAX] = {
    MICROPY_HW_I2C0_SDA, MICROPY_HW_I2C1_SDA,
};

// ---- Pin parse: int PinName, or string "PAx"/"PA_x"/"PBx"/"PB_x" --
// (same helper as machine_spi.c's machine_spi_get_pin() / machine_i2c.c's
// machine_i2c_get_pin() -- each pin-accepting constructor in this port
// keeps its own static copy rather than sharing one across files).
static PinName machine_i2c_target_get_pin(mp_obj_t obj) {
    if (mp_obj_is_int(obj)) {
        return (PinName)mp_obj_get_int(obj);
    }
    if (mp_obj_is_str(obj)) {
        size_t len;
        const char *s = mp_obj_str_get_data(obj, &len);
        if (len >= 3 && s[0] == 'P' && (s[1] == 'A' || s[1] == 'B')) {
            size_t i = 2;
            if (s[i] == '_') {
                i++;
            }
            int num = 0;
            bool has_digit = false;
            for (; i < len; i++) {
                if (s[i] < '0' || s[i] > '9') {
                    has_digit = false;
                    break;
                }
                num = num * 10 + (s[i] - '0');
                has_digit = true;
            }
            if (has_digit && num >= 0 && num <= 31) {
                int base = (s[1] == 'B') ? (int)PB_0 : (int)PA_0;
                return (PinName)(base + num);
            }
        }
    }
    mp_raise_ValueError(MP_ERROR_TEXT("invalid I2C pin"));
}

// ---------------------------------------------------------------------------
// Port object struct
// ---------------------------------------------------------------------------
typedef struct _machine_i2c_target_obj_t {
    mp_obj_base_t  base;
    i2c_t          i2c;          // mbed HAL object (handles pinmux/clock)
    I2C_TypeDef   *I2Cx;         // raw register pointer from I2C_DEV_TABLE
    uint8_t        i2c_id;
    uint8_t        state;
    bool           initialized;
    bool           stop_pending;
    bool           irq_active;
    PinName        scl;
    PinName        sda;
} machine_i2c_target_obj_t;

static machine_i2c_target_obj_t machine_i2c_target_obj[MICROPY_PY_MACHINE_I2C_TARGET_MAX];

// Forward declaration for mutual exclusion check against master I2C.
// machine_i2c_obj is declared non-static in machine_i2c.c.
typedef struct _machine_i2c_obj_t {
    mp_obj_base_t base;
    i2c_t    i2c;
    uint8_t  i2c_id;
    PinName  scl;
    PinName  sda;
    uint32_t freq;
    uint32_t timeout;
    bool     initialized;
} machine_i2c_obj_t;
extern machine_i2c_obj_t machine_i2c_obj[2];

// ---------------------------------------------------------------------------
// ISR — raw fwlib, registered via InterruptRegister
// ---------------------------------------------------------------------------
static u32 i2c_target_irq_handler(void *data) {
    machine_i2c_target_obj_t *self = (machine_i2c_target_obj_t *)data;
    machine_i2c_target_data_t *d = &machine_i2c_target_data[self->i2c_id];
    I2C_TypeDef *I2Cx = self->I2Cx;

    self->irq_active = true;
    u32 intr = I2C_GetINT(I2Cx);

    // TX abort (NAK from master, collision, etc.) — clear and recover.
    if (intr & I2C_BIT_R_TX_ABRT) {
        I2C_ClearINT(I2Cx, I2C_BIT_R_TX_ABRT);
    }

    // RX_DONE must be handled BEFORE STOP_DET below.  On this DesignWare I2C
    // IP, RX_DONE (master NACKed the last transmitted byte -- the read is
    // ending) arrives in the SAME interrupt poll as the STOP_DET that ends
    // that same read (confirmed on hardware: intr=0x00000280 = STOP_DET|
    // RX_DONE together for a 1-byte readfrom()).  If STOP_DET ran first, it
    // would already reset self->state to STATE_IDLE, and this RX_DONE would
    // then be misread as the START of a brand new transaction (spurious
    // extra IRQ_ADDR_MATCH_READ + IRQ_READ_REQ pair appearing AFTER
    // IRQ_END_READ).  Handling RX_DONE first keeps state == STATE_READING
    // for both this call and STOP_DET's finalisation right after it, so
    // read_request() is correctly treated as a continuation of the read that
    // is about to end, not a new one.
    //
    // RX_DONE fires as an EXTRA event beyond the real RD_REQ transfers: for a
    // 1-byte readfrom(), RD_REQ fires once (poll 1) and RX_DONE fires once
    // more (poll 2, alongside STOP_DET) -- matching upstream's own reference
    // behaviour (see rp2's port, which ORs RD_REQ and RX_DONE into the same
    // handler) and tests/extmod_hardware/machine_i2c_target.py's
    // TestIRQ.test_read (2 IRQ_READ_REQ for a 1-byte readfrom()) and
    // TestMemory's test_write_read_read (mem_addr advances one past the
    // last byte actually delivered).  Call the same read_request() path so
    // both the event count and the mem_addr bookkeeping match.
    if (intr & I2C_BIT_R_RX_DONE) {
        I2C_ClearINT(I2Cx, I2C_BIT_R_RX_DONE);
        bool was_reading = (self->state == STATE_READING);
        self->state = STATE_READING;
        if (!was_reading) {
            machine_i2c_target_data_addr_match(d, true);
        }
        machine_i2c_target_data_read_request(self, d);
    }

    // STOP_DET must be handled BEFORE RX_FULL/RD_REQ below.  On this
    // DesignWare I2C IP, a trailing STOP_DET for one sub-transaction (e.g.
    // the address-byte write phase of readfrom_mem()) can arrive in the SAME
    // interrupt poll as the RD_REQ/RX_FULL that starts the NEXT
    // sub-transaction (e.g. the repeated-START read phase).  Handling it
    // first finalises the old transaction (using the state that was live
    // before this call) so the new transaction's RX_FULL/RD_REQ below sees a
    // clean STATE_IDLE and fires exactly one address-match event, instead of
    // the new state being clobbered back to IDLE after it was just set.
    //
    // Guard against STOP arriving before the last RX byte is drained
    // (same race as rp2's stop_pending pattern — DesignWare I2C IP).
    if (intr & I2C_BIT_R_STOP_DET) {
        I2C_ClearINT(I2Cx, I2C_BIT_R_STOP_DET);
        if (I2C_CheckFlagState(I2Cx, I2C_BIT_RFNE)) {
            self->stop_pending = true;
        } else {
            // A zero-length write probe (e.g. SoftI2C.scan()'s address-only
            // START+address+STOP with no data byte) never raises RX_FULL, so
            // d->state is still STATE_IDLE here and no address-match event
            // would otherwise be recorded -- tests/extmod_hardware/
            // machine_i2c_target.py's test_scan cases expect one.
            //
            // STOP_DET itself can't be used to detect this (confirmed on
            // hardware: it fires for EVERY address on the bus, not just ours
            // -- 112/112 during a full scan()).  LP_WAKE_1 (bit 12 of
            // IC_RAW_INTR_STAT), documented as "Set when address SAR matches
            // with address sending on I2C BUS" (RTL8721Dx UM1000 v5.0,
            // ss 20.4.14), is the real per-address signal: confirmed on
            // hardware to fire on exactly 1 of 112 probes during a full
            // scan() -- precisely the one matching our own address.  Despite
            // the LP_WAKE ("low power wake") name suggesting a sleep-only
            // feature, it fires unconditionally, awake or not.
            //
            // Read-to-clear (IC_CLR_ADDR_MATCH) on every pass so a stale set
            // bit from an earlier probe can't be misread on a later one.
            uint32_t raw = I2C_GetRawINT(I2Cx);
            bool addr_matched = (raw & I2C_BIT_LP_WAKE_1) != 0;
            (void)I2Cx->IC_CLR_ADDR_MATCH;
            if (d->state == STATE_IDLE && addr_matched) {
                machine_i2c_target_data_addr_match(d, false);
            }
            machine_i2c_target_data_restart_or_stop(d);
            self->state = STATE_IDLE;
        }
    }

    // RX_FULL: master is writing a byte to us.
    if (intr & I2C_BIT_R_RX_FULL) {
        // RX_FULL is LEVEL-triggered on this DesignWare IP: it stays
        // asserted as long as the FIFO holds more bytes than IC_RX_TL, and
        // re-fires the ISR immediately on return if never cleared. Mask it
        // unconditionally here, before calling the event handlers below;
        // extmod's write_request() (called just below) always ends up
        // calling mp_machine_i2c_target_read_bytes() at least once -- either
        // automatically (mem_buf set, or mem_buf==NULL with no Python
        // handler registered) or via the user's own i2c_target.readinto()
        // inside their IRQ_WRITE_REQ handler -- and that function
        // unconditionally re-enables RX_FULL once it runs. The only way RX_FULL
        // stays masked past this ISR call is a handler that deliberately
        // defers the read to outside the IRQ (a documented, valid usage --
        // see tests/extmod_hardware/machine_i2c_target.py TestPolling), which
        // is exactly the case that must stay masked to avoid an infinite
        // storm (confirmed on hardware: 300k+ re-entries in under 6 seconds,
        // unresponsive to Ctrl-C, before this masking existed). A handler
        // that drains one byte per call but can't keep up with the master
        // (confirmed on hardware: a 5-byte writeto_mem() with a 400us-per-byte
        // handler leaves 4 more bytes queued by the time the first drains)
        // gets re-enabled by that same readinto() call and the level-
        // triggered interrupt immediately re-fires for the next byte instead
        // of losing it. Same pattern as rp2's ports/rp2/machine_i2c_target.c
        // i2c_target_handler()/mp_machine_i2c_target_read_bytes().
        I2C_INTConfig(I2Cx, I2C_BIT_M_RX_FULL, DISABLE);

        // Set state BEFORE calling the event handlers below: handle_event()
        // runs the user's Python IRQ callback, which may sleep (e.g. to force
        // clock stretching).  If the hardware re-latches RX_FULL for the next
        // byte during that window, the re-entrant ISR call must already see
        // STATE_WRITING, or it will wrongly re-fire IRQ_ADDR_MATCH_WRITE.
        bool was_writing = (self->state == STATE_WRITING);
        self->state = STATE_WRITING;
        if (!was_writing) {
            machine_i2c_target_data_addr_match(d, false);
        }
        machine_i2c_target_data_write_request(self, d);
    }

    // RD_REQ: master is requesting a byte from us.
    if (intr & I2C_BIT_R_RD_REQ) {
        I2C_ClearINT(I2Cx, I2C_BIT_R_RD_REQ);
        // Same early-state-update reasoning as the RX_FULL branch above,
        // for IRQ_ADDR_MATCH_READ re-entrancy during the RD_REQ event handler.
        bool was_reading = (self->state == STATE_READING);
        self->state = STATE_READING;
        if (!was_reading) {
            machine_i2c_target_data_addr_match(d, true);
        }
        machine_i2c_target_data_read_request(self, d);
    }

    self->irq_active = false;

    // Drain any byte that arrived simultaneously with STOP.
    if (self->stop_pending && !I2C_CheckFlagState(I2Cx, I2C_BIT_RFNE)) {
        self->stop_pending = false;
        self->state = STATE_IDLE;
        machine_i2c_target_data_restart_or_stop(d);
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Port hooks required by extmod/machine_i2c_target.c
// ---------------------------------------------------------------------------

static inline size_t mp_machine_i2c_target_get_index(machine_i2c_target_obj_t *self) {
    return self->i2c_id;
}

static inline void mp_machine_i2c_target_event_callback(machine_i2c_target_irq_obj_t *irq) {
    mp_irq_handler(&irq->base);
}

// Read bytes from hardware RX FIFO — non-blocking, called both from ISR
// context (auto-discard path) and directly from I2CTarget.readinto() (the
// caller-drains-later path — see the RX_FULL storm-prevention comment above).
static size_t mp_machine_i2c_target_read_bytes(machine_i2c_target_obj_t *self,
    size_t len, uint8_t *buf) {
    size_t i = 0;
    while (i < len && I2C_CheckFlagState(self->I2Cx, I2C_BIT_RFNE)) {
        buf[i++] = (uint8_t)(self->I2Cx->IC_DATA_CMD & 0xFF);
    }
    // Unconditionally re-enable, matching rp2. Safe even if the FIFO still
    // holds more bytes: RX_FULL is level-triggered, so the interrupt simply
    // reasserts immediately and the ISR runs again for the next byte.
    I2C_INTConfig(self->I2Cx, I2C_BIT_M_RX_FULL, ENABLE);
    return i;
}

// Write bytes to hardware TX FIFO — non-blocking, called from ISR context.
static size_t mp_machine_i2c_target_write_bytes(machine_i2c_target_obj_t *self,
    size_t len, const uint8_t *buf) {
    size_t i = 0;
    while (i < len && I2C_CheckFlagState(self->I2Cx, I2C_BIT_TFNF)) {
        I2C_SlaveSend(self->I2Cx, buf[i++]);
    }
    return i;
}

// Trigger filtering is done entirely in software by extmod's handle_event().
// Hardware interrupts stay enabled from construction to deinit — no dynamic
// enable/disable needed, matching rp2 and esp32 behavior.
static void mp_machine_i2c_target_irq_config(machine_i2c_target_obj_t *self,
    unsigned int trigger) {
    (void)self;
    (void)trigger;
}

// ---------------------------------------------------------------------------
// make_new
// ---------------------------------------------------------------------------
static mp_obj_t mp_machine_i2c_target_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {

    enum { ARG_id, ARG_addr, ARG_addrsize, ARG_mem, ARG_mem_addrsize,
           ARG_scl, ARG_sda };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_id,           MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_addr,         MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_addrsize,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 7} },
        { MP_QSTR_mem,          MP_ARG_KW_ONLY | MP_ARG_OBJ,
          {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_mem_addrsize, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 8} },
        { MP_QSTR_scl,          MP_ARG_KW_ONLY | MP_ARG_OBJ,
          {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_sda,          MP_ARG_KW_ONLY | MP_ARG_OBJ,
          {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    int i2c_id = args[ARG_id].u_int;
    if (i2c_id < 0 || i2c_id >= MICROPY_PY_MACHINE_I2C_TARGET_MAX) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("I2CTarget(%d) doesn't exist"), i2c_id);
    }

    if (args[ARG_addrsize].u_int != 7) {
        mp_raise_ValueError(MP_ERROR_TEXT("addrsize must be 7 (10-bit address not supported)"));
    }

    // Refuse if the same controller is active as master.
    if (machine_i2c_obj[i2c_id].initialized) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("I2C(%d) already in use as master"), i2c_id);
    }

    machine_i2c_target_obj_t *self = &machine_i2c_target_obj[i2c_id];
    if (self->initialized) {
        mp_machine_i2c_target_deinit(self);
    }
    MP_STATE_PORT(machine_i2c_target_irq_obj)[i2c_id] = NULL;
    MP_STATE_PORT(machine_i2c_target_mem_obj)[i2c_id] = MP_OBJ_NULL;
    self->base.type = &machine_i2c_target_type;
    self->i2c_id = (uint8_t)i2c_id;

    // Resolve SCL/SDA — fall back to board defaults if not given.
    PinName scl = (args[ARG_scl].u_obj != mp_const_none)
        ? machine_i2c_target_get_pin(args[ARG_scl].u_obj)
        : i2c_target_default_scl[i2c_id];
    PinName sda = (args[ARG_sda].u_obj != mp_const_none)
        ? machine_i2c_target_get_pin(args[ARG_sda].u_obj)
        : i2c_target_default_sda[i2c_id];
    self->scl = scl;
    self->sda = sda;

    // mbed HAL init: handles pinmux + PAD pull-up internally.
    // Must set i2c_idx before calling i2c_init (HAL uses it to index DEV_TABLE).
    self->i2c.i2c_idx = i2c_id;
    i2c_init(&self->i2c, sda, scl);
    i2c_slave_address(&self->i2c, 0, (uint32_t)args[ARG_addr].u_int, 0xFF);
    i2c_slave_mode(&self->i2c, 1);

    // Get raw register pointer from I2C_DEV_TABLE (same source as the
    // official raw_i2c_int_slave example).
    self->I2Cx = I2C_DEV_TABLE[i2c_id].I2Cx;

    // i2c_slave_mode() above re-inits via I2C_Init(), which loads
    // I2C_StructInit()'s default second slave address (IC_SAR2 = 0x12,
    // ameba_i2c.c) and never clears it. The DesignWare I2C IP has no bit to
    // disable SAR2 matching independently, so without this the target
    // phantom-ACKs 0x12 in addition to the caller's requested address
    // (confirmed on hardware: scan() returned [0x12, 0x42] for an
    // I2CTarget(addr=0x42)). Mirror the primary address onto SAR2 so both
    // registers agree instead of leaving the stale SDK default active.
    self->I2Cx->IC_SAR2 = ((uint32_t)args[ARG_addr].u_int) & I2C_MASK_IC_SAR2;

    // Init extmod state machine.
    self->state = STATE_IDLE;
    self->stop_pending = false;
    self->irq_active = false;
    MP_STATE_PORT(machine_i2c_target_mem_obj)[i2c_id] = args[ARG_mem].u_obj;
    machine_i2c_target_data_t *data = &machine_i2c_target_data[i2c_id];
    machine_i2c_target_data_init(data, args[ARG_mem].u_obj,
        args[ARG_mem_addrsize].u_int);

    // Enable all relevant slave interrupts once at construction; extmod's
    // handle_event() does the trigger filtering in software.
    I2C_INTConfig(self->I2Cx,
        I2C_BIT_M_RX_FULL | I2C_BIT_M_RD_REQ | I2C_BIT_M_RX_DONE |
        I2C_BIT_M_STOP_DET | I2C_BIT_M_TX_ABRT, ENABLE);

    // Register and enable hardware IRQ (same pattern as machine_uart.c).
    InterruptRegister((IRQ_FUN)i2c_target_irq_handler,
        I2C_DEV_TABLE[i2c_id].IrqNum, (u32)self, INT_PRI_MIDDLE);
    InterruptEn(I2C_DEV_TABLE[i2c_id].IrqNum, INT_PRI_MIDDLE);

    self->initialized = true;
    return MP_OBJ_FROM_PTR(self);
}

// ---------------------------------------------------------------------------
// print
// ---------------------------------------------------------------------------
static void mp_machine_i2c_target_print(const mp_print_t *print,
    mp_obj_t self_in, mp_print_kind_t kind) {
    machine_i2c_target_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "I2CTarget(%u, addr=%u, scl=%u, sda=%u)",
        self->i2c_id,
        (unsigned)I2C_GET_IC_SAR(self->I2Cx->IC_SAR),
        (unsigned)self->scl,
        (unsigned)self->sda);
}

// ---------------------------------------------------------------------------
// deinit
// ---------------------------------------------------------------------------
static void mp_machine_i2c_target_deinit(machine_i2c_target_obj_t *self) {
    if (!self->initialized) {
        return;
    }
    // Disable IRQ before touching hardware to prevent stale ISR firing.
    InterruptDis(I2C_DEV_TABLE[self->i2c_id].IrqNum);
    InterruptUnRegister(I2C_DEV_TABLE[self->i2c_id].IrqNum);
    I2C_INTConfig(self->I2Cx,
        I2C_BIT_M_RX_FULL | I2C_BIT_M_RD_REQ | I2C_BIT_M_RX_DONE |
        I2C_BIT_M_STOP_DET | I2C_BIT_M_TX_ABRT, DISABLE);
    i2c_slave_mode(&self->i2c, 0);
    Pinmux_Config((u8)self->scl, PINMUX_FUNCTION_GPIO);
    Pinmux_Config((u8)self->sda, PINMUX_FUNCTION_GPIO);
    self->initialized = false;
    self->state = STATE_INACTIVE;
}

// Called from mp_main.c soft-reset path (same pattern as machine_i2c.c).
void machine_i2c_target_deinit_all(void) {
    for (int i = 0; i < MICROPY_PY_MACHINE_I2C_TARGET_MAX; i++) {
        mp_machine_i2c_target_deinit(&machine_i2c_target_obj[i]);
    }
}
