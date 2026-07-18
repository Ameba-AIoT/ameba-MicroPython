// SPDX-License-Identifier: MIT
// machine.UART for ameba-rtos (AmebaDplus) — included by extmod/machine_uart.c
// via MICROPY_PY_MACHINE_UART_INCLUDEFILE.

#include "py/runtime.h"
#include "py/stream.h"
#include "py/mphal.h"
#include "py/ringbuf.h"

#include "serial_api.h"
#include "serial_ex_api.h"
#include "PinNames.h"
#include "ameba_uart.h"
#include "shared/runtime/mpirq.h"

// UART IRQ trigger constants (port-defined, matches esp32 naming/values).
#define UART_IRQ_RX      (1 << 0)
#define UART_IRQ_BREAK   (1 << 1)
#define UART_IRQ_RXIDLE  (1 << 2)
#define UART_IRQ_TXIDLE  (1 << 3)

#define UART_ID_COUNT (2)

#define UART_DEFAULT_BAUDRATE (115200)
#define UART_DEFAULT_BITS     (8)
#define UART_DEFAULT_STOP     (1)
#define UART_DEFAULT_RXBUF    (256)
#define UART_MIN_RXBUF        (32)
// Cap so that (rxbuf_len + 1) still fits the uint16_t ringbuf_t.size field
// (avoids a silent wrap to 0 → division-by-zero in ringbuf_put/avail).
#define UART_MAX_RXBUF        (8192)
#define UART_DEFAULT_TXBUF    (256)
#define UART_MIN_TXBUF        (32)
#define UART_MAX_TXBUF        (8192)

// Default UART pins — overridable per-board in mpconfigboard.h.
// Fallback values match PKE8721DAF board spec (Table 10 NOTE):
//   PA31 = UART_TX (Default), PA30 = UART_RX (Default).
// Both UART0 and UART1 accept any valid GPIO; there are no hardware-fixed
// pins.  The SDK's uart_tx_index_get() special-casing of PB31 is a legacy
// artefact; the correct pinmux (PINMUX_FUNCTION_UART0/1_TXD) is selected
// by the else-branch in serial_init() for any non-PB31 pin.
#ifndef MICROPY_HW_UART0_TX
#define MICROPY_HW_UART0_TX  (PA_31)
#endif
#ifndef MICROPY_HW_UART0_RX
#define MICROPY_HW_UART0_RX  (PA_30)
#endif
#ifndef MICROPY_HW_UART1_TX
#define MICROPY_HW_UART1_TX  (PA_29)
#endif
#ifndef MICROPY_HW_UART1_RX
#define MICROPY_HW_UART1_RX  (PA_28)
#endif

typedef struct _machine_uart_obj_t {
    mp_obj_base_t base;
    serial_t      serial;        // mbed HAL object, embedded (ISR holds pointer)
    uint8_t       uart_id;       // 0 or 1
    uint8_t       bits;          // 7 or 8
    uint8_t       parity;        // SerialParity enum value
    uint8_t       stop;          // 1 or 2
    uint32_t      baudrate;
    uint16_t      tx;            // PinName
    uint16_t      rx;            // PinName
    uint16_t      timeout;       // ms, total read timeout
    uint16_t      timeout_char;  // ms, inter-char timeout
    uint16_t      rxbuf_len;     // RX ringbuf capacity (excludes the +1 slot)
    uint16_t      txbuf_len;     // TX ringbuf capacity (excludes the +1 slot)
    ringbuf_t     read_buffer;   // RX ringbuf, filled by ISR
    ringbuf_t     write_buffer;  // TX ringbuf, drained into the HW FIFO by
                                  // write()/uart_fill_tx_fifo()/the TxIrq handler
    bool          tx_busy;       // true from write() until write_buffer *and*
                                  // the HW FIFO have both drained (see
                                  // uart_fill_tx_fifo() / machine_uart_irq_handler)
    bool          initialized;
    #if MICROPY_PY_MACHINE_UART_IRQ
    uint16_t      mp_irq_trigger;   // user-configured trigger mask
    uint16_t      mp_irq_flags;     // trigger flags from last ISR
    mp_irq_obj_t *mp_irq_obj;       // Python callback object
    #endif
} machine_uart_obj_t;

static machine_uart_obj_t machine_uart_obj[UART_ID_COUNT];

// GC roots for the RX/TX ring buffers (the static obj array is not GC-scanned).
MP_REGISTER_ROOT_POINTER(void *machine_uart_rxbuf[UART_ID_COUNT]);
MP_REGISTER_ROOT_POINTER(void *machine_uart_txbuf[UART_ID_COUNT]);

#define MICROPY_PY_MACHINE_UART_CLASS_CONSTANTS \
    { MP_ROM_QSTR(MP_QSTR_IRQ_RX), MP_ROM_INT(UART_IRQ_RX) }, \
    { MP_ROM_QSTR(MP_QSTR_IRQ_BREAK), MP_ROM_INT(UART_IRQ_BREAK) }, \
    { MP_ROM_QSTR(MP_QSTR_IRQ_RXIDLE), MP_ROM_INT(UART_IRQ_RXIDLE) }, \
    { MP_ROM_QSTR(MP_QSTR_IRQ_TXIDLE), MP_ROM_INT(UART_IRQ_TXIDLE) },

// ---- Task 2: Pin parse/validate ----

// Parse a tx/rx pin argument: accepts int PinName, or string "PAx"/"PA_x"/"PBx"/"PB_x".
static PinName machine_uart_get_pin(mp_obj_t obj) {
    if (mp_obj_is_int(obj)) {
        return (PinName)mp_obj_get_int(obj);
    }
    if (mp_obj_is_str(obj)) {
        size_t len;
        const char *s = mp_obj_str_get_data(obj, &len);
        // need at least "Px" + one digit, e.g. "PA7"
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
    mp_raise_ValueError(MP_ERROR_TEXT("invalid tx/rx pin"));
}

// Validate a UART pin.  Both UART0 and UART1 support the full flexible
// pinmux: any GPIO >= PA_6 (excluding non-existent PB_12) is valid.
// PA_0..PA_5 are excluded because they are bonded to MCM Flash internally.
static bool uart_tx_pin_ok(uint8_t id, PinName tx) {
    (void)id;
    return (tx >= PA_6) && (tx != PB_12);
}

static bool uart_rx_pin_ok(uint8_t id, PinName rx) {
    (void)id;
    return (rx >= PA_6) && (rx != PB_12);
}

// ---- Task 6: print ----

static void mp_machine_uart_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *parity = (self->parity == ParityNone) ? "None"
                       : (self->parity == ParityOdd)  ? "1" : "0";
    const char *txp = (self->tx < PB_0) ? "PA" : "PB";
    int txn = (self->tx < PB_0) ? (int)self->tx : (int)(self->tx - PB_0);
    const char *rxp = (self->rx < PB_0) ? "PA" : "PB";
    int rxn = (self->rx < PB_0) ? (int)self->rx : (int)(self->rx - PB_0);
    mp_printf(print, "UART(%u, baudrate=%u, bits=%u, parity=%s, stop=%u, tx=%s%d, rx=%s%d, rxbuf=%u, txbuf=%u, timeout=%u, timeout_char=%u)",
        self->uart_id, self->baudrate, self->bits, parity, self->stop,
        txp, txn, rxp, rxn, self->rxbuf_len, self->txbuf_len, self->timeout, self->timeout_char);
}

// ---- Task 3: RX ISR ----

// self->serial.uart_idx indexes the SDK's global UART_DEV_TABLE, which is how
// serial_api.c itself finds the register block -- see UART_DEV_TABLE usage in
// serial_init()/serial_irq_set() etc.
static inline UART_TypeDef *machine_uart_regs(machine_uart_obj_t *self) {
    return UART_DEV_TABLE[self->serial.uart_idx].UARTx;
}

// Push write_buffer into the HW TX FIFO, then (if there's more work left --
// either bytes still queued because the FIFO filled up, or we still owe a
// "truly done" notification) re-arm the one-shot TX-FIFO-empty interrupt.
// Ameba's RUART_BIT_ETBEI is edge/one-shot (the SDK auto-disables it again
// right before invoking machine_uart_irq_handler), unlike e.g. rp2's
// level-triggered PL011 TXIM which can just stay enabled -- so this must be
// re-armed on every refill, not only the first. Called from write() and from
// the TxIrq branch below.
static void uart_fill_tx_fifo(machine_uart_obj_t *self) {
    while (serial_writable(&self->serial) && ringbuf_avail(&self->write_buffer) > 0) {
        serial_putc(&self->serial, ringbuf_get(&self->write_buffer));
    }
    if (self->tx_busy) {
        serial_irq_set(&self->serial, TxIrq, 1);
    }
}

#if MICROPY_PY_MACHINE_UART_IRQ
// Keep the RX FIFO trigger level at 1 byte (needed for IRQ_RX to fire once
// per byte, per the official machine_uart_irq_rx.py contract) unless the user
// is listening for IRQ_RXIDLE. Per the Ameba UART user manual, the receiver
// timeout interrupt (ETOI/RUART_BIT_TIMEOUT_INT) only asserts while "at least
// one character is in the Rx FIFO" that hasn't been read yet -- at the
// default 1-byte trigger level, machine_uart_irq_handler's RxIrq branch drains
// every byte the instant it arrives, so that precondition can never hold and
// IRQ_RXIDLE can never fire. Raising the trigger level while IRQ_RXIDLE is
// armed lets short bursts sit unread long enough for the timeout condition to
// fire instead of being drained by IRQ_RX's own per-byte ERBI. Mirrors rp2's
// uart_set_irq_level(); see machine_uart_set_rx_fifo_level() callers.
static void machine_uart_set_rx_fifo_level(machine_uart_obj_t *self) {
    SerialFifoLevel level = (self->mp_irq_trigger & UART_IRQ_RXIDLE) ? FifoLvQuarter : FifoLv1Byte;
    serial_rx_fifo_level(&self->serial, level);
}
#endif

// RX/TX interrupt: drain HW FIFO into the ring buffer and detect IRQ_RX /
// IRQ_RXIDLE / IRQ_BREAK / IRQ_TXIDLE. ISR context — no mp_* calls.
static void machine_uart_irq_handler(uint32_t id, SerialIrq event) {
    if (id >= UART_ID_COUNT) {
        return;
    }
    machine_uart_obj_t *self = &machine_uart_obj[id];
    #if MICROPY_PY_MACHINE_UART_IRQ
    uint16_t flags = 0;
    #endif
    if (event == RxIrq) {
        // Drain the FIFO first, lagging enqueue by one byte so the *last*
        // byte pulled can conditionally be dropped instead of enqueued (see
        // below) -- then read LSR once, after draining, exactly like the
        // proven-working ordering this replaces. RUART_BIT_BREAK_INT (and
        // RUART_BIT_TIMEOUT_INT) only reliably show up in LSR once the FIFO
        // byte(s) they're tagging have actually been popped via serial_getc()
        // -- reading LSR *before* draining (tried first) never sees the
        // break flag at all.
        bool got_byte = false;
        int pending = -1;
        while (serial_readable(&self->serial)) {
            int c = serial_getc(&self->serial);
            if (pending >= 0) {
                // Drop on overflow (ringbuf_put returns -1 when full).
                ringbuf_put(&self->read_buffer, (uint8_t)pending);
                got_byte = true;
            }
            pending = c;
        }
        #if MICROPY_PY_MACHINE_UART_IRQ
        // serial_irq_set(RxIrq, 1) enables RUART_BIT_ERBI | RUART_BIT_ELSI |
        // RUART_BIT_ETOI together at the hardware level (see serial_api.c),
        // but the mbed serial_api layer only ever forwards this as a plain
        // RxIrq callback -- it never tells us *why* (new byte vs. receiver
        // timeout vs. break). Read the line-status register ourselves to
        // recover that: RUART_BIT_TIMEOUT_INT (ETOI) means the line went
        // idle after some bytes (IRQ_RXIDLE); RUART_BIT_BREAK_INT (part of
        // ELSI) means a break condition was seen (IRQ_BREAK) -- ISR-context
        // side note: this is a second LSR read within the same hardware
        // interrupt; that's safe here because the SDK's own uart_irqhandler()
        // (serial_api.c) works off a copy of LSR taken once at ISR entry, not
        // a fresh register read, so it isn't affected by what we clear here.
        uint32_t lsr = UART_LineStatusGet(machine_uart_regs(self));
        bool is_break = lsr & RUART_BIT_BREAK_INT;
        #else
        bool is_break = false;
        #endif
        // On a break condition this UART pushes a spurious 0x00 into the RX
        // FIFO at the same time it sets RUART_BIT_BREAK_INT, and unlike rp2's
        // DR register there is no per-byte error tag bundled with the data --
        // LSR only tells us a break happened somewhere in this drain batch,
        // not which byte. The break byte arrives after any real data already
        // queued ahead of it, so it's the one being held back in `pending` --
        // drop it instead of enqueueing it, otherwise it sits in read_buffer
        // as a phantom byte that permanently shifts every later read() by one
        // (see machine_uart_irq_break.py). This assumes at most one break
        // byte lands per interrupt; a break held long enough to push more
        // than one would still leak the earlier extras into read_buffer.
        if (pending >= 0 && !is_break) {
            ringbuf_put(&self->read_buffer, (uint8_t)pending);
            got_byte = true;
        }
        #if MICROPY_PY_MACHINE_UART_IRQ
        if (got_byte && (self->mp_irq_trigger & UART_IRQ_RX)) {
            flags |= UART_IRQ_RX;
        }
        if ((lsr & RUART_BIT_TIMEOUT_INT) && (self->mp_irq_trigger & UART_IRQ_RXIDLE)) {
            flags |= UART_IRQ_RXIDLE;
        }
        if (is_break && (self->mp_irq_trigger & UART_IRQ_BREAK)) {
            flags |= UART_IRQ_BREAK;
        }
        #endif
    } else if (event == TxIrq) {
        // serial_api.c's uart_txdone_callback() already disabled RUART_BIT_ETBEI
        // (one-shot) before calling us.
        if (ringbuf_avail(&self->write_buffer) > 0) {
            // More queued than fit in the FIFO on the last fill -- not done,
            // refill (which re-arms ETBEI) and keep quiet.
            uart_fill_tx_fifo(self);
        } else {
            // write_buffer empty *and* this TxIrq fired, i.e. the FIFO itself
            // just emptied too -- transmission genuinely finished.
            self->tx_busy = false;
            #if MICROPY_PY_MACHINE_UART_IRQ
            if (self->mp_irq_trigger & UART_IRQ_TXIDLE) {
                flags |= UART_IRQ_TXIDLE;
            }
            #endif
        }
    }
    #if MICROPY_PY_MACHINE_UART_IRQ
    if (flags && self->mp_irq_obj) {
        self->mp_irq_flags = flags;
        mp_irq_handler(self->mp_irq_obj);
    }
    #endif
}

// ---- Task 2 + Task 3: init_helper (Task 3 RX-integrated version) ----

static void mp_machine_uart_init_helper(machine_uart_obj_t *self, size_t n_args,
    const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_baudrate, ARG_bits, ARG_parity, ARG_stop, ARG_tx, ARG_rx,
           ARG_timeout, ARG_timeout_char, ARG_rxbuf, ARG_txbuf };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_baudrate, MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_bits,     MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_parity,   MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_stop,     MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_tx,       MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_rx,       MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_timeout,      MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_timeout_char, MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_rxbuf,        MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_txbuf,        MP_ARG_INT, {.u_int = -1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // First-time defaults (preserve existing values on re-init).
    if (!self->initialized) {
        self->baudrate = UART_DEFAULT_BAUDRATE;
        self->bits = UART_DEFAULT_BITS;
        self->parity = ParityNone;
        self->stop = UART_DEFAULT_STOP;
        self->timeout = 0;
        self->timeout_char = 0;
        self->rxbuf_len = UART_DEFAULT_RXBUF;
        self->txbuf_len = UART_DEFAULT_TXBUF;
        self->tx = (self->uart_id == 1) ? MICROPY_HW_UART1_TX : MICROPY_HW_UART0_TX;
        self->rx = (self->uart_id == 1) ? MICROPY_HW_UART1_RX : MICROPY_HW_UART0_RX;
    }

    if (args[ARG_baudrate].u_int > 0) {
        self->baudrate = args[ARG_baudrate].u_int;
    }
    if (args[ARG_bits].u_int >= 0) {
        if (args[ARG_bits].u_int != 7 && args[ARG_bits].u_int != 8) {
            mp_raise_ValueError(MP_ERROR_TEXT("bits must be 7 or 8"));
        }
        self->bits = args[ARG_bits].u_int;
    }
    if (args[ARG_parity].u_obj != MP_OBJ_NULL) {
        if (args[ARG_parity].u_obj == mp_const_none) {
            self->parity = ParityNone;
        } else {
            // MicroPython convention: 0 = even, 1 = odd.
            mp_int_t p = mp_obj_get_int(args[ARG_parity].u_obj);
            self->parity = (p & 1) ? ParityOdd : ParityEven;
        }
    }
    if (args[ARG_stop].u_int >= 0) {
        if (args[ARG_stop].u_int != 1 && args[ARG_stop].u_int != 2) {
            mp_raise_ValueError(MP_ERROR_TEXT("stop must be 1 or 2"));
        }
        self->stop = args[ARG_stop].u_int;
    }
    if (args[ARG_tx].u_obj != MP_OBJ_NULL) {
        self->tx = machine_uart_get_pin(args[ARG_tx].u_obj);
    }
    if (args[ARG_rx].u_obj != MP_OBJ_NULL) {
        self->rx = machine_uart_get_pin(args[ARG_rx].u_obj);
    }
    if (args[ARG_timeout].u_int >= 0) {
        self->timeout = args[ARG_timeout].u_int;
    }
    if (args[ARG_timeout_char].u_int >= 0) {
        self->timeout_char = args[ARG_timeout_char].u_int;
    }
    if (args[ARG_rxbuf].u_int >= 0) {
        mp_int_t v = args[ARG_rxbuf].u_int;
        if (v < UART_MIN_RXBUF) {
            v = UART_MIN_RXBUF;
        } else if (v > UART_MAX_RXBUF) {
            v = UART_MAX_RXBUF;
        }
        self->rxbuf_len = (uint16_t)v;
    }
    if (args[ARG_txbuf].u_int >= 0) {
        mp_int_t v = args[ARG_txbuf].u_int;
        if (v < UART_MIN_TXBUF) {
            v = UART_MIN_TXBUF;
        } else if (v > UART_MAX_TXBUF) {
            v = UART_MAX_TXBUF;
        }
        self->txbuf_len = (uint16_t)v;
    }

    // Validate pins against the requested id BEFORE touching the SDK.
    if (!uart_tx_pin_ok(self->uart_id, (PinName)self->tx)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid tx pin for this UART"));
    }
    if (!uart_rx_pin_ok(self->uart_id, (PinName)self->rx)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid rx pin for this UART"));
    }

    // Allocate the new RX/TX ring buffers BEFORE tearing down the old config,
    // so an OOM here leaves a previously-initialized instance fully intact:
    // the old buffers are still valid and still GC-rooted, and the
    // (still-armed) RX ISR keeps writing into read_buffer safely
    // (MicroPython's GC is non-moving).
    size_t cap = (size_t)self->rxbuf_len + 1;
    uint8_t *buf = m_new(uint8_t, cap);
    size_t tx_cap = (size_t)self->txbuf_len + 1;
    uint8_t *tx_buf = m_new(uint8_t, tx_cap);

    // Tear down a previous configuration on re-init. No allocation past this
    // point, so the half-applied state below cannot be unwound by an exception.
    if (self->initialized) {
        serial_irq_set(&self->serial, RxIrq, 0);
        serial_free(&self->serial);
        // self is machine_uart_obj[uart_id], reused in place rather than
        // freshly allocated -- clear the previous instance's IRQ
        // registration too. Otherwise mp_irq_trigger can still have
        // UART_IRQ_TXIDLE set from before, so this *new* instance's own
        // write() (see mp_machine_uart_write()) re-arms the one-shot TX
        // interrupt on the strength of that stale bit even though this
        // script never called .irq() -- and when it fires, it invokes
        // mp_irq_obj, a dangling pointer into the *previous* instance's
        // (possibly already GC-swept) Python callback. Reproducible hang:
        // machine_uart_irq_txidle.py followed by machine_uart_tx.py.
        self->mp_irq_obj = NULL;
        self->mp_irq_trigger = 0;
        self->tx_busy = false;
    }

    // Switch to the new ring buffers and re-root them (drops the old roots, if any).
    self->read_buffer.buf = buf;
    self->read_buffer.size = cap;
    self->read_buffer.iget = 0;
    self->read_buffer.iput = 0;
    MP_STATE_PORT(machine_uart_rxbuf)[self->uart_id] = buf;

    self->write_buffer.buf = tx_buf;
    self->write_buffer.size = tx_cap;
    self->write_buffer.iget = 0;
    self->write_buffer.iput = 0;
    MP_STATE_PORT(machine_uart_txbuf)[self->uart_id] = tx_buf;

    serial_init(&self->serial, (PinName)self->tx, (PinName)self->rx);
    serial_baud(&self->serial, self->baudrate);
    serial_format(&self->serial, self->bits, (SerialParity)self->parity, self->stop);

    // Register RX interrupt: id = uart_id so the ISR can find this instance.
    serial_irq_handler(&self->serial, machine_uart_irq_handler, self->uart_id);
    serial_irq_set(&self->serial, RxIrq, 1);
    #if MICROPY_PY_MACHINE_UART_IRQ
    // mp_irq_trigger was just reset above (or is 0 on first init), so this
    // sets the RX FIFO trigger level back to the IRQ_RX-friendly 1-byte
    // default -- see machine_uart_set_rx_fifo_level().
    machine_uart_set_rx_fifo_level(self);
    #endif

    self->initialized = true;
}

static mp_obj_t mp_machine_uart_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);
    mp_int_t uart_id = mp_obj_get_int(args[0]);
    if (uart_id < 0 || uart_id >= UART_ID_COUNT) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("UART(%d) doesn't exist"), (int)uart_id);
    }
    machine_uart_obj_t *self = &machine_uart_obj[uart_id];
    self->base.type = type;
    self->uart_id = (uint8_t)uart_id;
    // The Ameba HAL does NOT derive the peripheral from the pins: serial_init()
    // and every later HAL call index uart_adapter[]/UART_DEV_TABLE[] by
    // obj->uart_idx. Set it here or UART(1) would alias the UART0 peripheral.
    self->serial.uart_idx = (uint8_t)uart_id;
    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
    mp_machine_uart_init_helper(self, n_args - 1, args + 1, &kw_args);
    return MP_OBJ_FROM_PTR(self);
}

// ---- Task 6: deinit ----

static void mp_machine_uart_deinit(machine_uart_obj_t *self) {
    if (self->initialized) {
        serial_irq_set(&self->serial, RxIrq, 0);
        serial_free(&self->serial);
        self->initialized = false;
    }
    // machine_uart_obj[] is a static table indexed by uart_id, reused by the
    // next UART(id, ...) construction rather than freshly allocated -- clear
    // the IRQ registration too, not just the RX buffer, otherwise a stale
    // mp_irq_obj (already GC-swept, e.g. across a soft reset -- see
    // machine_uart_deinit_all() below) could get invoked by an ETBEI that was
    // still pending from the previous instance once the next UART instance's
    // serial_init() re-enables the NVIC line, before that instance's own
    // .irq() call has a chance to overwrite it. (Historically this was easy
    // to hit because write() armed ETBEI *after* fully draining write_buffer
    // by itself -- often when the FIFO was already empty, so it could never
    // self-fire; uart_fill_tx_fifo() no longer arms on an already-empty FIFO,
    // but tx_busy is still reset here as cheap defense-in-depth.)
    self->mp_irq_obj = NULL;
    self->mp_irq_trigger = 0;
    self->tx_busy = false;
}

// Called from the port soft-reset path (mp_main.c) BEFORE gc_sweep_all(): the RX
// ring buffers live in the GC heap, but the obj array is static BSS that survives
// soft reset with the SDK RX interrupt still enabled. Silence every UART IRQ and
// drop the dangling buffers so a byte arriving post-reset can't corrupt the heap.
void machine_uart_deinit_all(void) {
    for (int i = 0; i < UART_ID_COUNT; i++) {
        machine_uart_obj_t *self = &machine_uart_obj[i];
        if (self->initialized) {
            serial_irq_set(&self->serial, RxIrq, 0);
            serial_free(&self->serial);
            self->initialized = false;
        }
        self->read_buffer.buf = NULL;
        self->read_buffer.size = 0;
        self->read_buffer.iget = 0;
        self->read_buffer.iput = 0;
        self->write_buffer.buf = NULL;
        self->write_buffer.size = 0;
        self->write_buffer.iget = 0;
        self->write_buffer.iput = 0;
        // mp_irq_obj points into the GC heap that's about to be swept -- see
        // mp_machine_uart_deinit() for why this must be cleared, not just RX
        // state.
        self->mp_irq_obj = NULL;
        self->mp_irq_trigger = 0;
        self->tx_busy = false;
    }
}

// ---- Task 3: any() ----

static mp_int_t mp_machine_uart_any(machine_uart_obj_t *self) {
    return ringbuf_avail(&self->read_buffer);
}

// ---- Task 5: txdone() ----

static bool mp_machine_uart_txdone(machine_uart_obj_t *self) {
    if (!self->initialized) {
        return false;
    }
    // tx_busy is cleared by the TxIrq handler only once write_buffer *and*
    // the HW FIFO have both drained -- see uart_fill_tx_fifo()/
    // machine_uart_irq_handler().
    return !self->tx_busy;
}

// ---- Task 4: read with timeout ----

static mp_uint_t mp_machine_uart_read(mp_obj_t self_in, void *buf_in, mp_uint_t size, int *errcode) {
    machine_uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (size == 0) {
        return 0;
    }
    mp_uint_t start = mp_hal_ticks_ms();
    mp_uint_t timeout = self->timeout;
    uint8_t *dest = buf_in;

    for (size_t i = 0; i < size; i++) {
        while (ringbuf_avail(&self->read_buffer) == 0) {
            mp_uint_t elapsed = mp_hal_ticks_ms() - start;
            if (elapsed > timeout) {
                if (i == 0) {
                    *errcode = MP_EAGAIN;
                    return MP_STREAM_ERROR;
                }
                return i;
            }
            mp_event_wait_ms(1);
        }
        *dest++ = ringbuf_get(&self->read_buffer);
        start = mp_hal_ticks_ms();      // restart for inter-char timeout
        timeout = self->timeout_char;
    }
    return size;
}

// ---- Task 5: async write (queues to write_buffer, drained by TxIrq) ----

// Queues data into write_buffer and returns as soon as it's queued -- it does
// NOT wait for the data to actually leave the wire (use flush()/txdone() for
// that). This matches the upstream contract (e.g. rp2, esp32) and is what
// machine_uart_irq_txidle.py's timing model assumes: the previous
// byte-by-byte blocking implementation stayed inside write() for almost the
// entire transmission, so by the time it returned and armed the one-shot TX
// interrupt, the FIFO had *already* drained -- IRQ_TXIDLE fired immediately
// instead of asynchronously near the end of transmission, scrambling the
// expected output order (confirmed against the recorded .out/.exp mismatch).
static mp_uint_t mp_machine_uart_write(mp_obj_t self_in, const void *buf_in, mp_uint_t size, int *errcode) {
    machine_uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialized) {
        *errcode = MP_EINVAL;
        return MP_STREAM_ERROR;
    }
    if (size == 0) {
        return 0;
    }
    const uint8_t *src = buf_in;
    size_t i = 0;

    // Queue as many bytes as fit without blocking.
    while (i < size && ringbuf_free(&self->write_buffer) > 0) {
        ringbuf_put(&self->write_buffer, src[i]);
        i++;
    }
    self->tx_busy = true;
    uart_fill_tx_fifo(self);

    // write_buffer was smaller than `size` -- feed the rest in as room frees
    // up (mirrors rp2's mp_machine_uart_write()), cooperatively, no timeout
    // (this port has never had a write()-side timeout).
    while (i < size) {
        while (ringbuf_free(&self->write_buffer) == 0) {
            mp_event_handle_nowait();
        }
        ringbuf_put(&self->write_buffer, src[i]);
        i++;
        uart_fill_tx_fifo(self);
    }
    return size;
}

// ---- Task 4 + Task 5: ioctl (POLL + FLUSH) ----

static mp_uint_t mp_machine_uart_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    machine_uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_uint_t ret;
    if (request == MP_STREAM_POLL) {
        uintptr_t flags = arg;
        ret = 0;
        // After deinit the ringbuf may be torn down and the peripheral freed;
        // report neither readable nor writable rather than touch stale state.
        if (self->initialized) {
            if ((flags & MP_STREAM_POLL_RD) && ringbuf_avail(&self->read_buffer) > 0) {
                ret |= MP_STREAM_POLL_RD;
            }
            if ((flags & MP_STREAM_POLL_WR) && ringbuf_free(&self->write_buffer) > 0) {
                ret |= MP_STREAM_POLL_WR;
            }
        }
    } else if (request == MP_STREAM_FLUSH) {
        // Wait for tx_busy to clear, i.e. for the TxIrq handler to confirm
        // write_buffer *and* the HW FIFO have both drained -- this is driven
        // by the real ETBEI interrupt (see uart_fill_tx_fifo()), not a bare
        // LSR poll, so it doesn't hit AmebaGreen2's old TX_EMPTY-never-
        // latches-when-polled-without-ETBEI issue (that bug was specific to
        // reading the status bit without the interrupt enabled; still worth
        // re-confirming on real Green2 hardware after this rewrite).
        // Timeout sized for a full write_buffer + FIFO drain at the current
        // baudrate, doubled for margin.
        uint32_t timeout_us = ((uint32_t)self->txbuf_len + UART_TX_FIFO_SIZE + 1) * 10 * 1000000UL * 2 / self->baudrate;
        uint32_t start = mp_hal_ticks_us();
        while (self->tx_busy) {
            if (mp_hal_ticks_us() - start > timeout_us) {
                *errcode = MP_ETIMEDOUT;
                return MP_STREAM_ERROR;
            }
            mp_event_handle_nowait();
        }
        ret = 0;
    } else {
        *errcode = MP_EINVAL;
        ret = MP_STREAM_ERROR;
    }
    return ret;
}

// ---------------------------------------------------------------------------
// sendbreak — hold TX low for one character-time (break signal)
// ---------------------------------------------------------------------------
#if MICROPY_PY_MACHINE_UART_SENDBREAK
static void mp_machine_uart_sendbreak(machine_uart_obj_t *self) {
    // A break = TX held low for ≥ 1 frame (10-11 bit-periods at current baud).
    // serial_set_flow_control is not for this; use a manual delay via TX-low pulse.
    // Approximate: 13 bit-periods at current baudrate (11 for frame + 2 guard).
    uint32_t break_us = 13 * 1000000UL / self->baudrate;
    serial_break_set(&self->serial);
    mp_hal_delay_us(break_us);
    serial_break_clear(&self->serial);
}
#endif

// ---------------------------------------------------------------------------
// irq — register a Python callback for UART events
// ---------------------------------------------------------------------------
#if MICROPY_PY_MACHINE_UART_IRQ

static const mp_irq_methods_t machine_uart_irq_methods;

static mp_irq_obj_t *mp_machine_uart_irq(machine_uart_obj_t *self,
    bool any_args, mp_arg_val_t *args) {

    if (any_args) {
        // Disable existing IRQ before re-configuring.
        if (self->mp_irq_obj) {
            self->mp_irq_trigger = 0;
            self->mp_irq_obj = NULL;
        }

        mp_arg_val_t *a = args;
        // args layout per extmod/machine_uart.h:
        //   ARG_handler, ARG_trigger, ARG_hard (hard IRQ not supported here)
        mp_obj_t handler = a[0].u_obj;
        mp_uint_t trigger = (mp_uint_t)a[1].u_int;

        if (handler != mp_const_none && trigger != 0) {
            mp_irq_obj_t *irq = mp_irq_new(&machine_uart_irq_methods,
                MP_OBJ_FROM_PTR(self));
            irq->handler = handler;
            irq->ishard = false;
            self->mp_irq_obj = irq;
            self->mp_irq_trigger = (uint16_t)trigger;
        }
        machine_uart_set_rx_fifo_level(self);
    }

    if (self->mp_irq_obj == NULL) {
        // Return a dummy IRQ object if none configured.
        mp_irq_obj_t *irq = mp_irq_new(&machine_uart_irq_methods,
            MP_OBJ_FROM_PTR(self));
        irq->handler = mp_const_none;
        return irq;
    }
    return self->mp_irq_obj;
}

static mp_uint_t machine_uart_irq_trigger(mp_obj_t self_in, mp_uint_t new_trigger) {
    machine_uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->mp_irq_trigger = (uint16_t)new_trigger;
    machine_uart_set_rx_fifo_level(self);
    return 0;
}

static mp_uint_t machine_uart_irq_info(mp_obj_t self_in, mp_uint_t info_type) {
    machine_uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (info_type == MP_IRQ_INFO_FLAGS) {
        return self->mp_irq_flags;
    } else if (info_type == MP_IRQ_INFO_TRIGGERS) {
        return self->mp_irq_trigger;
    }
    return 0;
}

static const mp_irq_methods_t machine_uart_irq_methods = {
    .trigger  = machine_uart_irq_trigger,
    .info     = machine_uart_irq_info,
};

#endif // MICROPY_PY_MACHINE_UART_IRQ
