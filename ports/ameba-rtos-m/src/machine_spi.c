// SPDX-License-Identifier: MIT
// machine.SPI (hardware master) for ameba-rtos (AmebaDplus).
//
// Unlike machine.UART (which fills in extmod/machine_uart.c via an INCLUDEFILE),
// machine.SPI is a standalone type: this file defines the global machine_spi_type
// plus an mp_machine_spi_p_t protocol struct. extmod/machine_spi.c supplies the
// read/readinto/write/write_readinto/init/deinit method wrappers (and the MSB/LSB
// constants) through mp_machine_spi_locals_dict; extmod/modmachine.c registers
// SPI -> machine_spi_type under the MICROPY_PY_MACHINE_SPI gate.

#include "py/runtime.h"
#include "py/mphal.h"
#include "py/mperrno.h"
#include "extmod/modmachine.h"

#include "spi_api.h"
#include "PinNames.h"
#include "ameba_soc.h"   // Pinmux_Config(), PINMUX_FUNCTION_GPIO (SDK umbrella header)

#define SPI_ID_COUNT (2)

#define SPI_DEFAULT_BAUDRATE  (1000000)
#define SPI_DEFAULT_POLARITY  (0)
#define SPI_DEFAULT_PHASE     (0)
#define SPI_DEFAULT_BITS      (8)

// AmebaDplus SPI0 dedicated-pinmux group (PINMUX_FUNCTION_SPI). Of the five
// dedicated groups, four are muxed to boot flash (PA14-17) or OSPI/PSRAM
// (PA12/26-28, PA29-31/PB17, PB18-21); Group3 (PB23-26) is the ONLY group with no
// flash/OSPI/PSRAM overlap, so it is the safe default. The previous default
// (PA26/27/28/12) sat on OSPI/PSRAM pins and never transferred. Override via
// sck=/mosi=/miso=/cs=. (PB25/PB26 exceed the machine.Pin GPIO range but the SPI
// pinmux accepts them.)
#define SPI0_DEFAULT_SCK   (PB_23)   // SPI0_CLK
#define SPI0_DEFAULT_MOSI  (PB_24)   // SPI0_MOSI
#define SPI0_DEFAULT_MISO  (PB_25)   // SPI0_MISO
#define SPI0_DEFAULT_CS    (PB_26)   // SPI0_CS

typedef struct _machine_spi_obj_t {
    mp_obj_base_t base;
    spi_t    spi;          // mbed HAL object, embedded
    uint8_t  spi_id;       // 0 or 1
    uint8_t  polarity;     // CPOL 0/1
    uint8_t  phase;        // CPHA 0/1
    uint8_t  bits;         // 8 only
    uint8_t  firstbit;     // MSB only
    uint32_t baudrate;
    uint16_t sck;          // PinName
    uint16_t mosi;         // PinName
    uint16_t miso;         // PinName
    uint16_t cs;           // PinName (hardware chip-select)
    bool     initialized;
} machine_spi_obj_t;

static machine_spi_obj_t machine_spi_obj[SPI_ID_COUNT];

// ---- Pin parse: int PinName, or string "PAx"/"PA_x"/"PBx"/"PB_x" ----

static PinName machine_spi_get_pin(mp_obj_t obj) {
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
    mp_raise_ValueError(MP_ERROR_TEXT("invalid SPI pin"));
}

// ---- print ----

static void machine_spi_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_spi_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *sckp = (self->sck < PB_0) ? "PA" : "PB";
    int sckn = (self->sck < PB_0) ? (int)self->sck : (int)(self->sck - PB_0);
    const char *mosip = (self->mosi < PB_0) ? "PA" : "PB";
    int mosin = (self->mosi < PB_0) ? (int)self->mosi : (int)(self->mosi - PB_0);
    const char *misop = (self->miso < PB_0) ? "PA" : "PB";
    int mison = (self->miso < PB_0) ? (int)self->miso : (int)(self->miso - PB_0);
    const char *csp = (self->cs < PB_0) ? "PA" : "PB";
    int csn = (self->cs < PB_0) ? (int)self->cs : (int)(self->cs - PB_0);
    mp_printf(print, "SPI(%u, baudrate=%u, polarity=%u, phase=%u, bits=%u, firstbit=%u, sck=%s%d, mosi=%s%d, miso=%s%d, cs=%s%d)",
        self->spi_id, self->baudrate, self->polarity, self->phase, self->bits, self->firstbit,
        sckp, sckn, mosip, mosin, misop, mison, csp, csn);
}

// ---- init_helper: (re)configure the peripheral ----

static void mp_machine_spi_init_helper(machine_spi_obj_t *self, size_t n_args,
    const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_baudrate, ARG_polarity, ARG_phase, ARG_bits, ARG_firstbit,
           ARG_sck, ARG_mosi, ARG_miso, ARG_cs };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_baudrate, MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_polarity, MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_phase,    MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_bits,     MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_firstbit, MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_sck,      MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_mosi,     MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_miso,     MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_cs,       MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // First-time defaults (preserve existing values on re-init).
    if (!self->initialized) {
        self->baudrate = SPI_DEFAULT_BAUDRATE;
        self->polarity = SPI_DEFAULT_POLARITY;
        self->phase = SPI_DEFAULT_PHASE;
        self->bits = SPI_DEFAULT_BITS;
        self->firstbit = MICROPY_PY_MACHINE_SPI_MSB;
        self->sck = SPI0_DEFAULT_SCK;
        self->mosi = SPI0_DEFAULT_MOSI;
        self->miso = SPI0_DEFAULT_MISO;
        self->cs = SPI0_DEFAULT_CS;
    }

    if (args[ARG_baudrate].u_int > 0) {
        self->baudrate = args[ARG_baudrate].u_int;
    }
    if (args[ARG_polarity].u_int >= 0) {
        self->polarity = args[ARG_polarity].u_int ? 1 : 0;
    }
    if (args[ARG_phase].u_int >= 0) {
        self->phase = args[ARG_phase].u_int ? 1 : 0;
    }
    if (args[ARG_bits].u_int >= 0) {
        if (args[ARG_bits].u_int != 8) {
            mp_raise_NotImplementedError(MP_ERROR_TEXT("bits must be 8"));
        }
        self->bits = 8;
    }
    if (args[ARG_firstbit].u_int >= 0) {
        if (args[ARG_firstbit].u_int != MICROPY_PY_MACHINE_SPI_MSB) {
            mp_raise_NotImplementedError(MP_ERROR_TEXT("firstbit must be MSB"));
        }
        self->firstbit = MICROPY_PY_MACHINE_SPI_MSB;
    }
    if (args[ARG_sck].u_obj != MP_OBJ_NULL) {
        self->sck = machine_spi_get_pin(args[ARG_sck].u_obj);
    }
    if (args[ARG_mosi].u_obj != MP_OBJ_NULL) {
        self->mosi = machine_spi_get_pin(args[ARG_mosi].u_obj);
    }
    if (args[ARG_miso].u_obj != MP_OBJ_NULL) {
        self->miso = machine_spi_get_pin(args[ARG_miso].u_obj);
    }
    if (args[ARG_cs].u_obj != MP_OBJ_NULL) {
        self->cs = machine_spi_get_pin(args[ARG_cs].u_obj);
    }

    // Tear down a previous configuration before re-init.
    if (self->initialized) {
        spi_free(&self->spi);
        self->initialized = false;
    }

    // The Ameba HAL indexes ssi_adapter_g[]/SPI_DEV_TABLE[] by obj->spi_idx and
    // asserts unless it is MBED_SPI0/MBED_SPI1 — set it BEFORE spi_init (see
    // spi_api.c spi_init() attention note), mirroring the UART uart_idx trap.
    self->spi.spi_idx = (self->spi_id == 1) ? MBED_SPI1 : MBED_SPI0;

    spi_init(&self->spi, (PinName)self->mosi, (PinName)self->miso,
        (PinName)self->sck, (PinName)self->cs);
    // SPI mode number = (CPOL << 1) | CPHA.
    spi_format(&self->spi, self->bits, (self->polarity << 1) | self->phase, 0);
    spi_frequency(&self->spi, (int)self->baudrate);

    self->initialized = true;
}

static mp_obj_t machine_spi_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);
    mp_int_t spi_id = mp_obj_get_int(args[0]);
    if (spi_id < 0 || spi_id >= SPI_ID_COUNT) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("SPI(%d) doesn't exist"), (int)spi_id);
    }
    machine_spi_obj_t *self = &machine_spi_obj[spi_id];
    self->base.type = type;
    self->spi_id = (uint8_t)spi_id;
    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
    mp_machine_spi_init_helper(self, n_args - 1, args + 1, &kw_args);
    return MP_OBJ_FROM_PTR(self);
}

// ---- protocol: init / deinit / transfer ----

static void machine_spi_init(mp_obj_base_t *self_in, size_t n_args,
    const mp_obj_t *pos_args, mp_map_t *kw_args) {
    machine_spi_obj_t *self = (machine_spi_obj_t *)self_in;
    mp_machine_spi_init_helper(self, n_args, pos_args, kw_args);
}

// The SDK's spi_free() only disables the peripheral; it leaves the four pads
// muxed to PINMUX_FUNCTION_SPI. Restore them to GPIO so they can be reused as
// machine.Pin (and so a later re-init/reconfig starts from a clean pinmux).
static void machine_spi_pins_release(machine_spi_obj_t *self) {
    Pinmux_Config((u8)self->sck, PINMUX_FUNCTION_GPIO);
    Pinmux_Config((u8)self->mosi, PINMUX_FUNCTION_GPIO);
    Pinmux_Config((u8)self->miso, PINMUX_FUNCTION_GPIO);
    Pinmux_Config((u8)self->cs, PINMUX_FUNCTION_GPIO);
}

static void machine_spi_deinit(mp_obj_base_t *self_in) {
    machine_spi_obj_t *self = (machine_spi_obj_t *)self_in;
    if (self->initialized) {
        spi_free(&self->spi);
        machine_spi_pins_release(self);
        self->initialized = false;
    }
}

// Full-duplex, blocking, per-frame transfer. extmod always passes src != NULL:
//   dest == NULL -> write only (received byte discarded)
//   src == dest  -> read   (extmod pre-fills the buffer with the write byte;
//                   we read src[i] before overwriting dest[i], so aliasing is safe)
//   src != dest  -> write_readinto
static void machine_spi_transfer(mp_obj_base_t *self_in, size_t len,
    const uint8_t *src, uint8_t *dest) {
    machine_spi_obj_t *self = (machine_spi_obj_t *)self_in;
    if (!self->initialized) {
        mp_raise_OSError(MP_EINVAL);
    }
    for (size_t i = 0; i < len; i++) {
        int out = (src != NULL) ? src[i] : 0xFF;
        int in = spi_master_write(&self->spi, out);
        if (dest != NULL) {
            dest[i] = (uint8_t)in;
        }
    }
}

static const mp_machine_spi_p_t machine_spi_p = {
    .init = machine_spi_init,
    .deinit = machine_spi_deinit,
    .transfer = machine_spi_transfer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    machine_spi_type,
    MP_QSTR_SPI,
    MP_TYPE_FLAG_NONE,
    make_new, machine_spi_make_new,
    print, machine_spi_print,
    protocol, &machine_spi_p,
    locals_dict, &mp_machine_spi_locals_dict
    );

// Called from the port soft-reset path (mp_main.c): the obj array is static BSS
// that survives soft reset with the SDK SPI peripheral still enabled. Free every
// instance so a re-init after reset starts from a clean peripheral state.
void machine_spi_deinit_all(void) {
    for (int i = 0; i < SPI_ID_COUNT; i++) {
        machine_spi_obj_t *self = &machine_spi_obj[i];
        if (self->initialized) {
            spi_free(&self->spi);
            machine_spi_pins_release(self);
            self->initialized = false;
        }
    }
}
