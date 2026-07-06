// SPDX-License-Identifier: MIT
// machine.UART for ameba-rtos (AmebaDplus) — included by extmod/machine_uart.c
// via MICROPY_PY_MACHINE_UART_INCLUDEFILE.

#include "py/runtime.h"
#include "py/stream.h"
#include "py/mphal.h"
#include "py/ringbuf.h"

#include "serial_api.h"
#include "PinNames.h"
#include "shared/runtime/mpirq.h"

// UART IRQ trigger constant (port-defined, matches esp32 naming).
#define UART_IRQ_RX  (1)

#define UART_ID_COUNT (2)

#define UART_DEFAULT_BAUDRATE (115200)
#define UART_DEFAULT_BITS     (8)
#define UART_DEFAULT_STOP     (1)
#define UART_DEFAULT_RXBUF    (256)
#define UART_MIN_RXBUF        (32)
// Cap so that (rxbuf_len + 1) still fits the uint16_t ringbuf_t.size field
// (avoids a silent wrap to 0 → division-by-zero in ringbuf_put/avail).
#define UART_MAX_RXBUF        (8192)

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
    ringbuf_t     read_buffer;   // RX ringbuf, filled by ISR
    bool          initialized;
    #if MICROPY_PY_MACHINE_UART_IRQ
    uint16_t      mp_irq_trigger;   // user-configured trigger mask
    uint16_t      mp_irq_flags;     // trigger flags from last ISR
    mp_irq_obj_t *mp_irq_obj;       // Python callback object
    #endif
} machine_uart_obj_t;

static machine_uart_obj_t machine_uart_obj[UART_ID_COUNT];

// GC root for the RX ring buffers (the static obj array is not GC-scanned).
MP_REGISTER_ROOT_POINTER(void *machine_uart_rxbuf[UART_ID_COUNT]);

// Only IRQ_RX is implemented in this phase (RXIDLE/BREAK/TXIDLE left for later).
#define MICROPY_PY_MACHINE_UART_CLASS_CONSTANTS \
    { MP_ROM_QSTR(MP_QSTR_IRQ_RX), MP_ROM_INT(UART_IRQ_RX) },

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
    mp_printf(print, "UART(%u, baudrate=%u, bits=%u, parity=%s, stop=%u, tx=%s%d, rx=%s%d, rxbuf=%u, timeout=%u, timeout_char=%u)",
        self->uart_id, self->baudrate, self->bits, parity, self->stop,
        txp, txn, rxp, rxn, self->rxbuf_len, self->timeout, self->timeout_char);
}

// ---- Task 3: RX ISR ----

// RX interrupt: drain HW FIFO into the ring buffer. ISR context — no mp_* calls.
static void machine_uart_irq_handler(uint32_t id, SerialIrq event) {
    if (id >= UART_ID_COUNT) {
        return;
    }
    machine_uart_obj_t *self = &machine_uart_obj[id];
    if (event == RxIrq) {
        bool got_byte = false;
        while (serial_readable(&self->serial)) {
            int c = serial_getc(&self->serial);
            // Drop on overflow (ringbuf_put returns -1 when full).
            ringbuf_put(&self->read_buffer, (uint8_t)c);
            got_byte = true;
        }
        #if MICROPY_PY_MACHINE_UART_IRQ
        // Only fire the Python callback if this invocation actually drained
        // a byte, in case RxIrq is ever raised with the FIFO already empty.
        if (got_byte && self->mp_irq_obj && (self->mp_irq_trigger & UART_IRQ_RX)) {
            self->mp_irq_flags = UART_IRQ_RX;
            mp_irq_handler(self->mp_irq_obj);
        }
        #endif
    }
}

// ---- Task 2 + Task 3: init_helper (Task 3 RX-integrated version) ----

static void mp_machine_uart_init_helper(machine_uart_obj_t *self, size_t n_args,
    const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_baudrate, ARG_bits, ARG_parity, ARG_stop, ARG_tx, ARG_rx,
           ARG_timeout, ARG_timeout_char, ARG_rxbuf };
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

    // Validate pins against the requested id BEFORE touching the SDK.
    if (!uart_tx_pin_ok(self->uart_id, (PinName)self->tx)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid tx pin for this UART"));
    }
    if (!uart_rx_pin_ok(self->uart_id, (PinName)self->rx)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid rx pin for this UART"));
    }

    // Allocate the new RX ring buffer BEFORE tearing down the old config, so an
    // OOM here leaves a previously-initialized instance fully intact: the old
    // buffer is still valid and still GC-rooted, and the (still-armed) RX ISR
    // keeps writing into it safely (MicroPython's GC is non-moving).
    size_t cap = (size_t)self->rxbuf_len + 1;
    uint8_t *buf = m_new(uint8_t, cap);

    // Tear down a previous configuration on re-init. No allocation past this
    // point, so the half-applied state below cannot be unwound by an exception.
    if (self->initialized) {
        serial_irq_set(&self->serial, RxIrq, 0);
        serial_free(&self->serial);
    }

    // Switch to the new ring buffer and re-root it (drops the old root, if any).
    self->read_buffer.buf = buf;
    self->read_buffer.size = cap;
    self->read_buffer.iget = 0;
    self->read_buffer.iput = 0;
    MP_STATE_PORT(machine_uart_rxbuf)[self->uart_id] = buf;

    serial_init(&self->serial, (PinName)self->tx, (PinName)self->rx);
    serial_baud(&self->serial, self->baudrate);
    serial_format(&self->serial, self->bits, (SerialParity)self->parity, self->stop);

    // Register RX interrupt: id = uart_id so the ISR can find this instance.
    serial_irq_handler(&self->serial, machine_uart_irq_handler, self->uart_id);
    serial_irq_set(&self->serial, RxIrq, 1);

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
    // Blocking write() pushes each byte into the FIFO as space frees up, so by
    // the time write() returns the data is queued. Report ready when the FIFO
    // can accept more (best-effort; mbed serial exposes no TX-empty flag).
    return serial_writable(&self->serial) != 0;
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

// ---- Task 5: blocking write ----

static mp_uint_t mp_machine_uart_write(mp_obj_t self_in, const void *buf_in, mp_uint_t size, int *errcode) {
    machine_uart_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialized) {
        *errcode = MP_EINVAL;
        return MP_STREAM_ERROR;
    }
    const uint8_t *src = buf_in;
    for (size_t i = 0; i < size; i++) {
        // Wait (cooperatively) until the TX FIFO can accept a byte.
        while (!serial_writable(&self->serial)) {
            mp_event_handle_nowait();
        }
        serial_putc(&self->serial, src[i]);
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
            if ((flags & MP_STREAM_POLL_WR) && serial_writable(&self->serial)) {
                ret |= MP_STREAM_POLL_WR;
            }
        }
    } else if (request == MP_STREAM_FLUSH) {
        // Blocking TX already drained bytes into the FIFO during write().
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
