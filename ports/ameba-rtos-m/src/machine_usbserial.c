// ports/ameba-rtos-m/src/machine_usbserial.c
//
// SPDX-License-Identifier: MIT
//
// machine.USBSerial -- USB CDC-ACM virtual serial port, EV8711FLM
// (AmebaGreen2) only. Binds ameba-rtos's own usbd_cdc_acm vendor
// class-driver stack (component/usb/device/{core,cdc_acm}), not upstream's
// TinyUSB-backed machine.USBDevice -- this port has no viable TinyUSB DCD
// for either board's USB controller (see
// docs/superpowers/specs/2026-07-19-p4-usb-vcp.md for the full comparison).
//
// Initialization sequence and per-board FIFO depths are copied from the
// vendor's own reference example,
// ameba-rtos/example/usb/usbd_cdc_acm/example_usbd_cdc_acm.c.
#include "py/runtime.h"
#include "py/stream.h"
#include "py/mperrno.h"
#include "py/ringbuf.h"
#include "py/mpthread.h"
#include "ameba_soc.h"
#include "os_wrapper.h"
#include "usbd_cdc_acm.h"

#include "machine_usbserial.h"

#if MICROPY_PY_MACHINE_USBSERIAL

/* ---------- RX: ISR (received callback) -> ring buffer ------------------ */
//
// Separate from mphalport.c's stdin_ringbuf -- that one's contract is "sole
// producer: log_uart_irq" (see its comment there); sharing it with a second
// ISR producer would break that single-producer/single-consumer invariant.
static uint8_t usbserial_rx_array[256];
static ringbuf_t usbserial_rx_ringbuf = {usbserial_rx_array, sizeof(usbserial_rx_array), 0, 0};
static volatile uint32_t usbserial_rx_overflow;

/* ---------- TX: blocking helper, shared by write() and the REPL mirror -- */
//
// usbd_cdc_acm_transmit() hands our buffer pointer straight to DMA
// (CONFIG_CDC_ACM_BULK_TX_SKIP_MEMCPY=1, the usbd_cdc_acm.h default --
// confirmed by reading usbd_cdc_acm.c's usbd_cdc_acm_transmit(): with this
// flag set it assigns ep_bulk_in->xfer_buf = buf directly, no memcpy).
// Every caller must therefore wait for the `transmitted` callback before
// touching/reusing its buffer -- there is no other way to know the DMA is
// done reading it.
//
// This helper is shared by USBSerial.write() and the REPL mirror
// (mp_usbserial_tx_strn()) precisely so every accepted transmit is paired
// with exactly one rtos_sema_take() on usbserial_tx_done_sema. That pairing
// alone is not enough once a take can time out, though: if caller A's wait
// times out while its transfer is still physically in flight, the eventual
// `transmitted` callback still fires and leaves a stale, unconsumed give on
// the semaphore -- a later caller B's own take could then consume that
// stale token instead of its own completion, and return success while B's
// own DMA read of its buffer is still in progress. usbserial_tx_mutex
// serializes all callers so only one transmit is ever being negotiated at a
// time, and the non-blocking drain loop below clears any such stale token
// before a new transmit is issued, so a caller's own take can only be
// satisfied by its own transfer's completion.
static rtos_mutex_t usbserial_tx_mutex;
static rtos_sema_t usbserial_tx_done_sema;

static bool usbserial_transmit_blocking(const uint8_t *buf, uint32_t len, uint32_t timeout_ms) {
    if (rtos_mutex_take(usbserial_tx_mutex, timeout_ms) != RTK_SUCCESS) {
        return false;
    }

    // Clear any stale completion token left behind by an earlier caller
    // whose own wait below timed out while its transfer was still
    // physically in flight (see the comment above this function).
    while (rtos_sema_take(usbserial_tx_done_sema, 0) == RTK_SUCCESS) {
    }

    int ret = usbd_cdc_acm_transmit((u8 *)buf, len);
    if (ret == HAL_BUSY) {
        // Defensive only -- should not happen now that usbserial_tx_mutex
        // serializes every caller of this helper, but stay safe: wait for
        // whatever's in flight, then retry once.
        rtos_sema_take(usbserial_tx_done_sema, timeout_ms);
        ret = usbd_cdc_acm_transmit((u8 *)buf, len);
    }

    bool ok;
    if (ret != HAL_OK) {
        ok = false;
    } else {
        ok = rtos_sema_take(usbserial_tx_done_sema, timeout_ms) == RTK_SUCCESS;
    }

    rtos_mutex_give(usbserial_tx_mutex);
    return ok;
}

/* ---------- attach state -------------------------------------------------- */
static volatile bool usbserial_attached;

/* ---------- usbd_cdc_acm_cb_t callbacks (ISR context, see usbd_cdc_acm.h) - */

static int usbserial_cb_init(void) {
    return HAL_OK;
}

static int usbserial_cb_deinit(void) {
    return HAL_OK;
}

static int usbserial_cb_setup(usb_setup_req_t *req, u8 *buf) {
    (void)req;
    (void)buf;
    return HAL_OK;
}

static int usbserial_cb_received(u8 *buf, u32 len) {
    // usbd_cdc_acm.c re-arms the OUT endpoint (usbd_ep_receive()) right
    // after this callback returns -- no action needed here beyond draining
    // buf into our ring buffer (confirmed by reading usbd_cdc_acm.c).
    for (u32 i = 0; i < len; i++) {
        if (ringbuf_put(&usbserial_rx_ringbuf, buf[i]) < 0) {
            ++usbserial_rx_overflow;
        }
    }
    return HAL_OK;
}

static void usbserial_cb_transmitted(u8 status) {
    (void)status;
    rtos_sema_give(usbserial_tx_done_sema);
}

static void usbserial_cb_status_changed(u8 old_status, u8 status) {
    (void)old_status;
    usbserial_attached = (status == USBD_ATTACH_STATUS_ATTACHED);
}

static usbd_cdc_acm_cb_t usbserial_cb = {
    .init = usbserial_cb_init,
    .deinit = usbserial_cb_deinit,
    .setup = usbserial_cb_setup,
    .received = usbserial_cb_received,
    .transmitted = usbserial_cb_transmitted,
    .status_changed = usbserial_cb_status_changed,
};

// rx_fifo_depth/ptx_fifo_depth are the AmebaGreen2 values from
// example_usbd_cdc_acm.c's usbd_config_t (CONFIG_AMEBAGREEN2 branch) --
// copied verbatim, not derived, since they depend on this SoC's actual USB
// FIFO hardware limits.
static usbd_config_t usbserial_usbd_cfg = {
    .speed = USB_SPEED_HIGH,
    .isr_priority = INT_PRI_MIDDLE,
    .rx_fifo_depth = 644U,
    .ptx_fifo_depth = {16U, 256U, 32U, 16U, 16U},
};

#define USBSERIAL_BULK_OUT_XFER_SIZE   2048U
#define USBSERIAL_BULK_IN_XFER_SIZE    2048U
#define USBSERIAL_TX_TIMEOUT_MS        500U   // USBSerial.write() -- Task 2
#define USBSERIAL_REPL_TX_TIMEOUT_MS   20U    // REPL mirror -- Task 3; bounds
                                               // worst-case LOGUART console
                                               // stall if a USB host is
                                               // attached but not consuming.

static bool usbserial_core_ready;

void machine_usbserial_init0(void) {
    if (usbserial_core_ready) {
        return;
    }
    if (rtos_sema_create(&usbserial_tx_done_sema, 0, 1) != RTK_SUCCESS) {
        return;
    }
    if (rtos_mutex_create(&usbserial_tx_mutex) != RTK_SUCCESS) {
        rtos_sema_delete(usbserial_tx_done_sema);
        return;
    }
    if (usbd_init(&usbserial_usbd_cfg) != HAL_OK) {
        rtos_mutex_delete(usbserial_tx_mutex);
        rtos_sema_delete(usbserial_tx_done_sema);
        return;
    }
    if (usbd_cdc_acm_init(USBSERIAL_BULK_OUT_XFER_SIZE, USBSERIAL_BULK_IN_XFER_SIZE, &usbserial_cb) != HAL_OK) {
        usbd_deinit();
        rtos_mutex_delete(usbserial_tx_mutex);
        rtos_sema_delete(usbserial_tx_done_sema);
        return;
    }
    usbserial_core_ready = true;
}

/* ---------- singleton object ---------------------------------------------- */

typedef struct _machine_usbserial_obj_t {
    mp_obj_base_t base;
} machine_usbserial_obj_t;

static machine_usbserial_obj_t machine_usbserial_obj = {
    .base = { &machine_usbserial_type },
};

static mp_obj_t machine_usbserial_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    enum { ARG_id };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_id, MP_ARG_INT, {.u_int = 0} },
    };

    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, args, &kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    if (parsed[ARG_id].u_int != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("USBSerial(id) must be 0 -- this board has one USB device controller"));
    }

    // The core driver is already up (machine_usbserial_init0() ran at boot,
    // see mp_main.c) unless a prior deinit() tore it down -- try again here
    // so USBSerial() after deinit()/USBSerial() still works.
    machine_usbserial_init0();

    return MP_OBJ_FROM_PTR(&machine_usbserial_obj);
}

static mp_obj_t machine_usbserial_init(mp_obj_t self_in) {
    (void)self_in;
    machine_usbserial_init0();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_usbserial_init_obj, machine_usbserial_init);

// Caveat: deinit() must not be called concurrently with a write() still in
// flight on another thread -- deleting usbserial_tx_done_sema/
// usbserial_tx_mutex while another thread is blocked inside
// usbserial_transmit_blocking() on them is a use-after-delete. This port has
// no existing guard against that class of hazard for any driver;
// documenting it here rather than adding one-off locking.
static mp_obj_t machine_usbserial_deinit(mp_obj_t self_in) {
    (void)self_in;
    if (usbserial_core_ready) {
        // Also tears down the REPL mirror (Task 3) -- same class of
        // deliberate side effect as deinit()-ing the UART LOGUART is bound
        // to, documented in the spec's known limitations.
        usbd_cdc_acm_deinit();
        usbd_deinit();
        rtos_mutex_delete(usbserial_tx_mutex);
        rtos_sema_delete(usbserial_tx_done_sema);
        usbserial_core_ready = false;
        usbserial_attached = false;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_usbserial_deinit_obj, machine_usbserial_deinit);

static mp_obj_t machine_usbserial_isconnected(mp_obj_t self_in) {
    (void)self_in;
    return mp_obj_new_bool(usbserial_attached);
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_usbserial_isconnected_obj, machine_usbserial_isconnected);

static mp_obj_t machine_usbserial_any(mp_obj_t self_in) {
    (void)self_in;
    return mp_obj_new_bool(ringbuf_peek(&usbserial_rx_ringbuf) != -1);
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_usbserial_any_obj, machine_usbserial_any);

static mp_uint_t machine_usbserial_stream_read(mp_obj_t self_in, void *buf_in, mp_uint_t size, int *errcode) {
    (void)self_in;
    uint8_t *buf = (uint8_t *)buf_in;
    mp_uint_t got = 0;
    while (got < size) {
        int c = ringbuf_get(&usbserial_rx_ringbuf);
        if (c == -1) {
            break;
        }
        buf[got++] = (uint8_t)c;
    }
    if (got == 0) {
        *errcode = MP_EAGAIN;
        return MP_STREAM_ERROR;
    }
    return got;
}

static mp_uint_t machine_usbserial_stream_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    (void)self_in;
    if (!usbserial_core_ready || !usbserial_attached) {
        // Not connected -- return 0 bytes written rather than raising or
        // blocking, matching pyb_usb_vcp_send()'s disconnected behaviour on
        // stm32 (there's no host to receive anything, and no way for a
        // detached endpoint to ever signal completion).
        return 0;
    }
    MP_THREAD_GIL_EXIT();
    bool ok = usbserial_transmit_blocking((const uint8_t *)buf, (uint32_t)size, USBSERIAL_TX_TIMEOUT_MS);
    MP_THREAD_GIL_ENTER();
    if (!ok) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    return size;
}

static mp_uint_t machine_usbserial_stream_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    (void)self_in;
    if (request == MP_STREAM_POLL) {
        uintptr_t flags = arg;
        mp_uint_t ret = 0;
        if ((flags & MP_STREAM_POLL_RD) && ringbuf_peek(&usbserial_rx_ringbuf) != -1) {
            ret |= MP_STREAM_POLL_RD;
        }
        if (flags & MP_STREAM_POLL_WR) {
            ret |= MP_STREAM_POLL_WR;
        }
        return ret;
    } else if (request == MP_STREAM_CLOSE) {
        // USB attach/detach is host-controlled -- close() must not tear
        // down the device, same reasoning as stm32's USB_VCP.close() being
        // a no-op (mp_identity_obj there).
        return 0;
    }
    *errcode = MP_EINVAL;
    return MP_STREAM_ERROR;
}

static const mp_stream_p_t machine_usbserial_stream_p = {
    .read = machine_usbserial_stream_read,
    .write = machine_usbserial_stream_write,
    .ioctl = machine_usbserial_stream_ioctl,
};

static const mp_rom_map_elem_t machine_usbserial_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init),        MP_ROM_PTR(&machine_usbserial_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),      MP_ROM_PTR(&machine_usbserial_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_isconnected), MP_ROM_PTR(&machine_usbserial_isconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_any),         MP_ROM_PTR(&machine_usbserial_any_obj) },
    { MP_ROM_QSTR(MP_QSTR_read),        MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto),    MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline),    MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write),       MP_ROM_PTR(&mp_stream_write_obj) },
};
static MP_DEFINE_CONST_DICT(machine_usbserial_locals_dict, machine_usbserial_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_usbserial_type,
    MP_QSTR_USBSerial,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    make_new, machine_usbserial_make_new,
    protocol, &machine_usbserial_stream_p,
    locals_dict, &machine_usbserial_locals_dict
    );

/* ---------- mphalport.c HAL hooks (REPL mirror) --------------------------- */

uintptr_t mp_usbserial_poll(uintptr_t poll_flags) {
    uintptr_t ret = 0;
    if ((poll_flags & MP_STREAM_POLL_RD) && ringbuf_peek(&usbserial_rx_ringbuf) != -1) {
        ret |= MP_STREAM_POLL_RD;
    }
    if (poll_flags & MP_STREAM_POLL_WR) {
        ret |= MP_STREAM_POLL_WR;
    }
    return ret;
}

int mp_usbserial_rx_chr(void) {
    return ringbuf_get(&usbserial_rx_ringbuf);
}

void mp_usbserial_tx_strn(const char *str, size_t len) {
    if (!usbserial_core_ready || !usbserial_attached) {
        return;
    }
    // Best-effort mirror -- a short timeout (USBSERIAL_REPL_TX_TIMEOUT_MS,
    // Task 1) bounds how long a stalled/non-consuming USB host can hold up
    // the LOGUART console; a dropped chunk here is a cosmetic USB-mirror
    // glitch, not a LOGUART regression. Failures are swallowed rather than
    // surfaced, matching mp_os_dupterm_tx_strn's fire-and-forget contract --
    // LOGUART stays the console of record either way. usbserial_transmit_
    // blocking() can block on this timeout up to three times in sequence
    // (the tx_mutex acquisition, the defensive HAL_BUSY retry wait, and the
    // final completion wait), so the true worst-case LOGUART/VM stall is
    // roughly 3x this value, not a hard single-timeout bound.
    usbserial_transmit_blocking((const uint8_t *)str, (uint32_t)len, USBSERIAL_REPL_TX_TIMEOUT_MS);
}

#endif // MICROPY_PY_MACHINE_USBSERIAL
