// SPDX-License-Identifier: MIT
// machine.I2C (hardware master) for ameba-rtos (AmebaDplus).
//
// Implements the mp_machine_i2c_p_t protocol via transfer_single, using the
// mbed HAL i2c_write/i2c_read for single-buffer transfers.  extmod's
// mp_machine_i2c_transfer_adaptor handles multi-buffer consolidation and
// the readfrom_mem/writeto_mem combined-transaction fallback.
//
// The Ameba SDK i2c_start/i2c_stop are no-ops (the HAL manages START/STOP
// internally inside i2c_write/i2c_read).

#include "py/runtime.h"
#include "py/mphal.h"
#include "py/mperrno.h"
#include "extmod/modmachine.h"

#include "i2c_api.h"
#include "PinNames.h"
#include "ameba_soc.h"   // Pinmux_Config(), PINMUX_FUNCTION_GPIO

// i2c_write_timeout() is defined in the SDK's hal/src/i2c_api.c but NOT
// declared in hal/include/i2c_api.h.  We need it for zero-length writes
// (scan probe): the non-timeout i2c_write() with len=0 is a no-op on the
// bus (I2C_MasterWrite loop doesn't execute), returning 0 unconditionally,
// which makes scan() see every addressed as ACKed (false positives).
// i2c_write_timeout() properly calls I2C_MasterSendNullData_TimeOut() which
// sends START + address + STOP and detects ACK/NACK.
//
// i2c_read_timeout()/i2c_write_timeout() are also used for the actual data
// phases: the non-timeout i2c_read()/i2c_write() call I2C_MasterRead()/
// I2C_MasterWrite(), which busy-wait on the bus with NO upper bound.  A
// slave that holds SCL/SDA low (e.g. mid-transaction, or wired wrong) then
// hangs the interpreter forever.  The _TimeOut variants bound the wait and
// reset+recover the peripheral on failure, returning a short count so we can
// surface -EIO/-ETIMEDOUT instead of locking up.
extern int i2c_write_timeout(i2c_t *obj, int address, char *data, int length, int stop, int timeout_ms);
extern int i2c_read_timeout(i2c_t *obj, int address, char *data, int length, int stop, int timeout_ms);

#define I2C_ID_COUNT       (2)
#define DEFAULT_I2C_FREQ   (400000)
// Per-transfer timeout (ms) when the user does not pass timeout=.  Generous
// enough for a full 100-byte buffer at 100 kHz (~10 ms) yet short enough that
// a stuck bus surfaces an error instead of an unbounded hang.
#define DEFAULT_I2C_TIMEOUT_MS (1000)

typedef struct _machine_i2c_obj_t {
    mp_obj_base_t base;
    i2c_t    i2c;          // mbed HAL object, embedded
    uint8_t  i2c_id;       // 0 or 1
    PinName  scl;
    PinName  sda;
    uint32_t freq;
    uint32_t timeout;      // per-transfer timeout in ms
    bool     initialized;
} machine_i2c_obj_t;

static machine_i2c_obj_t machine_i2c_obj[I2C_ID_COUNT];

// ---- Pin parse: int PinName, or string "PAx"/"PA_x"/"PBx"/"PB_x" ----

static PinName machine_i2c_get_pin(mp_obj_t obj) {
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

// ---- print ----

static void machine_i2c_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_i2c_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *sclp = (self->scl < PB_0) ? "PA" : "PB";
    int scln = (self->scl < PB_0) ? (int)self->scl : (int)(self->scl - PB_0);
    const char *sdap = (self->sda < PB_0) ? "PA" : "PB";
    int sdan = (self->sda < PB_0) ? (int)self->sda : (int)(self->sda - PB_0);
    mp_printf(print, "I2C(%u, freq=%u, scl=%s%d, sda=%s%d)",
        self->i2c_id, self->freq, sclp, scln, sdap, sdan);
}

// ---- init_helper: (re)configure the peripheral ----

static void mp_machine_i2c_init_helper(machine_i2c_obj_t *self,
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    enum { ARG_freq, ARG_scl, ARG_sda, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_freq,    MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_scl,     MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_sda,     MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // First-time defaults (preserve existing values on re-init).
    if (!self->initialized) {
        self->freq = DEFAULT_I2C_FREQ;
        self->timeout = DEFAULT_I2C_TIMEOUT_MS;
        // scl/sda have no board-default; they must be set by the user the
        // first time, so leave them uninitialised — if they are still NULL
        // after parsing we raise ValueError below.
        self->scl = (PinName)0;
        self->sda = (PinName)0;
    }

    if (args[ARG_freq].u_int > 0) {
        self->freq = (uint32_t)args[ARG_freq].u_int;
    }
    if (args[ARG_scl].u_obj != MP_OBJ_NULL) {
        self->scl = machine_i2c_get_pin(args[ARG_scl].u_obj);
    }
    if (args[ARG_sda].u_obj != MP_OBJ_NULL) {
        self->sda = machine_i2c_get_pin(args[ARG_sda].u_obj);
    }
    if (args[ARG_timeout].u_int > 0) {
        self->timeout = (uint32_t)args[ARG_timeout].u_int;
    }

    // Both pins must be assigned before init.
    if (self->scl == (PinName)0 || self->sda == (PinName)0) {
        mp_raise_ValueError(MP_ERROR_TEXT("scl and sda must be specified"));
    }

    // Tear down a previous configuration before re-init.
    if (self->initialized) {
        // i2c_reset() only disables the peripheral (I2C_Cmd(DISABLE)).
        // The pinmux will be re-assigned by i2c_init() below.
        i2c_reset(&self->i2c);
        self->initialized = false;
    }

    // The Ameba HAL indexes I2C_DEV_TABLE[] by obj->i2c_idx — set it BEFORE
    // i2c_init (same trap as SPI's spi_idx and UART's uart_idx).
    self->i2c.i2c_idx = self->i2c_id;

    // i2c_init handles pinmux and pull-up internally:
    //   I2C0: Pinmux_Config(sda, PINMUX_FUNCTION_I2C0_SDA) + PAD_PullCtrl(UP)
    //         Pinmux_Config(scl, PINMUX_FUNCTION_I2C0_SCL) + PAD_PullCtrl(UP)
    //   I2C1: PINMUX_FUNCTION_I2C1_SDA / I2C1_SCL
    i2c_init(&self->i2c, (PinName)self->sda, (PinName)self->scl);
    i2c_frequency(&self->i2c, (int)self->freq);

    // Work around an Ameba HAL address-cache bug: i2c_api.c keeps the last
    // target address in a file-static (i2c_target_addr[]) and only reloads the
    // hardware IC_TAR register when the address *changes*.  i2c_init() above
    // resets IC_TAR but leaves that static stale, so the first transfer to an
    // address that a previous I2C object already used would skip the reload and
    // address the wrong slave (TX abort: 7B_ADDR_NOACK -> EIO).  Prime the cache
    // with an invalid 7-bit address (0xFF) so the first real transfer to any
    // 0x00-0x7F address is guaranteed to miss the cache and reload IC_TAR.  This
    // is a generic fix (any device/address), not specific to any slave; the cost
    // is one harmless zero-length probe to reserved address 0x7F at construction.
    i2c_write_timeout(&self->i2c, 0xFF, NULL, 0, 1, 1);

    self->initialized = true;
}

// ---- make_new ----

static mp_obj_t machine_i2c_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *args) {

    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);
    mp_int_t i2c_id = mp_obj_get_int(args[0]);

    if (i2c_id < 0 || i2c_id >= I2C_ID_COUNT) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("I2C(%d) doesn't exist"), (int)i2c_id);
    }

    machine_i2c_obj_t *self = &machine_i2c_obj[i2c_id];
    self->base.type = type;
    self->i2c_id = (uint8_t)i2c_id;

    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
    mp_machine_i2c_init_helper(self, n_args - 1, args + 1, &kw_args);

    return MP_OBJ_FROM_PTR(self);
}

// ---- protocol: init / deinit / transfer ----

static void machine_i2c_init(mp_obj_base_t *self_in,
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    machine_i2c_obj_t *self = (machine_i2c_obj_t *)self_in;
    mp_machine_i2c_init_helper(self, n_args, pos_args, kw_args);
}

// The SDK's i2c_reset() only disables the peripheral (I2C_Cmd(DISABLE));
// it leaves the two pads muxed to I2C.  Restore them to GPIO so they can
// be reused as machine.Pin.  (Note: i2c_init handles pinmux assignment,
// so a re-init via init_helper → i2c_reset + i2c_init is also safe.)
//
// This function is static; it is called from both machine_i2c_deinit and
// the soft-reset deinit_all path.
static void machine_i2c_pins_release(machine_i2c_obj_t *self) {
    Pinmux_Config((u8)self->scl, PINMUX_FUNCTION_GPIO);
    Pinmux_Config((u8)self->sda, PINMUX_FUNCTION_GPIO);
}

static void machine_i2c_deinit(mp_obj_base_t *self_in) {
    machine_i2c_obj_t *self = (machine_i2c_obj_t *)self_in;
    if (self->initialized) {
        i2c_reset(&self->i2c);
        machine_i2c_pins_release(self);
        self->initialized = false;
    }
}

// Full-duplex, blocking, single-buffer transfer.  extmod's
// mp_machine_i2c_transfer_adaptor merges multi-buffer transfers
// (e.g. writeto_mem → write-address + write-data) before calling this.
//
// Note: the SDK's i2c_read()/i2c_write() ignore the `stop` parameter and
// always send STOP after each transfer.  Combined transactions (repeated
// START between write and read phases) are NOT supported at the hardware
// level in this implementation; extmod falls back to two separate
// transactions for readfrom_mem/writeto_mem, which works with most I2C
// devices.  Upgrade to i2c_repeatread() + transfer_supports_write1 if a
// device requires true repeated START.
static int machine_i2c_transfer_single(mp_obj_base_t *self_in,
    uint16_t addr, size_t len, uint8_t *buf, unsigned int flags) {

    machine_i2c_obj_t *self = (machine_i2c_obj_t *)self_in;
    if (!self->initialized) {
        return -MP_EINVAL;
    }

    bool nostop = !(flags & MP_MACHINE_I2C_FLAG_STOP);
    int ret;

    if (flags & MP_MACHINE_I2C_FLAG_READ) {
        ret = i2c_read_timeout(&self->i2c, (int)addr, (char *)buf, (int)len, (int)(!nostop), (int)self->timeout);
        if (ret != (int)len) {
            return -MP_EIO;
        }
        return (int)len;
    } else {
        if (len == 0) {
            // Zero-length write (used by scan() to probe a 7-bit address).
            // i2c_write() with len=0 does nothing on the bus (the HAL
            // I2C_MasterWrite loop doesn't execute), so it always returns
            // 0 regardless of whether any slave ACKed, causing scan() to
            // report false positives for every address 0x08-0x77.
            // Use i2c_write_timeout which calls
            // I2C_MasterSendNullData_TimeOut to actually send
            // START + address + STOP and detect ACK/NACK.
            // The timeout of 100 ms is generous for a single probe.
            ret = i2c_write_timeout(&self->i2c, (int)addr, NULL, 0, (int)(!nostop), 100);
            return (ret < 0) ? -MP_ENODEV : 0;
        }
        ret = i2c_write_timeout(&self->i2c, (int)addr, (char *)buf, (int)len, (int)(!nostop), (int)self->timeout);
        if (ret != (int)len) {
            return -MP_EIO;
        }
        return (int)len;
    }
}

static const mp_machine_i2c_p_t machine_i2c_p = {
    .init = machine_i2c_init,
    .transfer = mp_machine_i2c_transfer_adaptor,
    .transfer_single = machine_i2c_transfer_single,
};

MP_DEFINE_CONST_OBJ_TYPE(
    machine_i2c_type,
    MP_QSTR_I2C,
    MP_TYPE_FLAG_NONE,
    make_new, machine_i2c_make_new,
    print, machine_i2c_print,
    protocol, &machine_i2c_p,
    locals_dict, &mp_machine_i2c_locals_dict
    );

// Called from the port soft-reset path (mp_main.c): the obj array is static
// BSS that survives soft reset with the SDK I2C peripheral still enabled.
// Free every instance so a re-init after reset starts from a clean state.
void machine_i2c_deinit_all(void) {
    for (int i = 0; i < I2C_ID_COUNT; i++) {
        machine_i2c_obj_t *self = &machine_i2c_obj[i];
        if (self->initialized) {
            i2c_reset(&self->i2c);
            machine_i2c_pins_release(self);
            self->initialized = false;
        }
    }
}
