// SPDX-License-Identifier: MIT
// machine.ADC port implementation for ameba-rtos.
//
// INCLUDEFILE — included by extmod/machine_adc.c via
// MICROPY_PY_MACHINE_ADC_INCLUDEFILE.  Defines machine_adc_obj_t and all
// mp_machine_adc_* functions required by the extmod framework.
//
// 8 channels (CH0-CH7) via the SDK's AD_0..AD_7 PinName aliases. On
// AmebaDplus, AD_7 is VBAT_MEAS, an internal channel with no external pin
// (see the CONFIG_AMEBADPLUS guard in mp_machine_adc_deinit below); on other
// SoCs all 8 channels are normal external pins.

// Must be defined (can be empty) for the extmod locals dict.
#define MICROPY_PY_MACHINE_ADC_CLASS_CONSTANTS

#include "analogin_api.h"
#include "ameba_soc.h"       // Pinmux_Config, PINMUX_FUNCTION_GPIO

// ---------------------------------------------------------------------------
// Pin → channel mapping (mirrors SDK's PinMap_ADC[])
// ---------------------------------------------------------------------------
typedef struct {
    PinName pin;
    uint8_t channel;
} adc_pin_map_t;

// AD_0..AD_7 are the SDK's own PinName aliases for the real hardware pins
// (see each SoC's PinNames.h / analogin_api.c PinMap_ADC[]), so this table
// tracks per-SoC pin assignments automatically instead of hardcoding them.
static const adc_pin_map_t adc_pin_table[] = {
    {AD_0, 0},
    {AD_1, 1},
    {AD_2, 2},
    {AD_3, 3},
    {AD_4, 4},
    {AD_5, 5},
    {AD_6, 6},
    {AD_7, 7},          // AD_7 == VBAT_MEAS (internal) on AmebaDplus
    {NC, 0xFF},         // terminator
};

#define ADC_PIN_COUNT 8       // AD_0..AD_7 (AD_7 == VBAT_MEAS on AmebaDplus)

static int8_t adc_pin_to_channel(PinName pin) {
    for (const adc_pin_map_t *p = adc_pin_table; p->pin != NC; p++) {
        if (p->pin == pin) {
            return (int8_t)p->channel;
        }
    }
    return -1;
}

static PinName adc_channel_to_pin(uint8_t channel) {
    for (const adc_pin_map_t *p = adc_pin_table; p->pin != NC; p++) {
        if (p->channel == channel) {
            return p->pin;
        }
    }
    return NC;
}

// ---------------------------------------------------------------------------
// Object type
// ---------------------------------------------------------------------------
typedef struct _machine_adc_obj_t {
    mp_obj_base_t base;
    analogin_t adc;          // mbed HAL object, embedded
    uint8_t    channel;      // 0-7
    PinName    pin;
    bool       initialized;
} machine_adc_obj_t;

// ---------------------------------------------------------------------------
// print
// ---------------------------------------------------------------------------
static void mp_machine_adc_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_adc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "ADC(%u)", self->channel);
}

// ---------------------------------------------------------------------------
// make_new — ADC(pin) or ADC(channel)
// ---------------------------------------------------------------------------
static mp_obj_t mp_machine_adc_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *args) {

    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    mp_obj_t source = args[0];

    PinName pin = NC;
    uint8_t channel;
    int8_t ch;

    if (mp_obj_is_int(source)) {
        // Integer: ADC(channel), e.g. ADC(0) → AD_0 = PB19
        mp_int_t c = mp_obj_get_int(source);
        if (c < 0 || c > 7) {
            mp_raise_ValueError(MP_ERROR_TEXT("ADC channel out of range"));
        }
        channel = (uint8_t)c;
        pin = adc_channel_to_pin(channel);
        if (pin == NC) {
            mp_raise_ValueError(MP_ERROR_TEXT("ADC channel out of range"));
        }
    } else {
        // Pin object / pin name: ADC(Pin("PB19")), ADC("PB19").
        // mp_hal_pin_resolve validates the type via machine_pin_find and
        // raises a clean ValueError on a non-pin argument, instead of
        // blindly dereferencing it as a pointer (undefined behaviour).
        pin = mp_hal_pin_resolve(source);
        ch = adc_pin_to_channel(pin);
        if (ch < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("Pin doesn't have ADC capabilities"));
        }
        channel = (uint8_t)ch;
    }

    // Dynamic allocation.
    machine_adc_obj_t *self = mp_obj_malloc(machine_adc_obj_t, &machine_adc_type);
    self->channel = channel;
    self->pin = pin;

    // analogin_init handles pinmux and ADC configuration internally.
    // Unlike SPI/UART/I2C/PWM, the SDK sets obj->adc_idx automatically
    // via pinmap_peripheral() — no need to pre-set it.
    analogin_init(&self->adc, pin);
    self->initialized = true;

    return MP_OBJ_FROM_PTR(self);
}

// ---------------------------------------------------------------------------
// read_u16 — 0..65535
// ---------------------------------------------------------------------------
static mp_int_t mp_machine_adc_read_u16(machine_adc_obj_t *self) {
    if (!self->initialized) {
        return 0;
    }
    // The hardware ADC is 12-bit; analogin_read_u16() returns the raw 12-bit
    // value (0-4095).  Scale to 16-bit (0-65535) using the same expansion as
    // the RP2 port: upper bits = raw, lower bits = interpolated copy.
    uint16_t raw = analogin_read_u16(&self->adc);
    const uint32_t bits = 12;
    return (mp_int_t)((raw << (16 - bits)) | (raw >> (2 * bits - 16)));
}

// ---------------------------------------------------------------------------
// deinit
// ---------------------------------------------------------------------------
static void mp_machine_adc_deinit(machine_adc_obj_t *self) {
    if (self->initialized) {
        analogin_deinit(&self->adc);
        #if defined(CONFIG_AMEBADPLUS)
        // VBAT_MEAS is an internal measurement channel with no external pin;
        // analogin_init skips Pinmux_Config for it, so we skip restoration too.
        if (self->pin != VBAT_MEAS) {
            Pinmux_Config((u8)self->pin, PINMUX_FUNCTION_GPIO);
        }
        #else
        Pinmux_Config((u8)self->pin, PINMUX_FUNCTION_GPIO);
        #endif
        self->initialized = false;
    }
}
