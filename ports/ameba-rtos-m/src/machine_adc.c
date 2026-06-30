// SPDX-License-Identifier: MIT
// machine.ADC port implementation for ameba-rtos (AmebaDplus).
//
// INCLUDEFILE — included by extmod/machine_adc.c via
// MICROPY_PY_MACHINE_ADC_INCLUDEFILE.  Defines machine_adc_obj_t and all
// mp_machine_adc_* functions required by the extmod framework.
//
// ADC pins: PB19 (CH0), PB18 (CH1), PB17 (CH2), PB16 (CH3),
// PB15 (CH4), PB14 (CH5), PB13 (CH6), VBAT_MEAS (CH7, internal).

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

static const adc_pin_map_t adc_pin_table[] = {
    {PB_19, 0},         // AD_0 = CH0
    {PB_18, 1},         // AD_1 = CH1
    {PB_17, 2},         // AD_2 = CH2
    {PB_16, 3},         // AD_3 = CH3
    {PB_15, 4},         // AD_4 = CH4
    {PB_14, 5},         // AD_5 = CH5
    {PB_13, 6},         // AD_6 = CH6
    {VBAT_MEAS, 7},     // CH7 = VBAT (internal, no external pin)
    {NC, 0xFF},         // terminator
};

#define ADC_PIN_COUNT 7       // 0-6 are external pins; CH7 = VBAT internal

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
        // VBAT_MEAS is an internal measurement channel with no external pin;
        // analogin_init skips Pinmux_Config for it, so we skip restoration too.
        if (self->pin != VBAT_MEAS) {
            Pinmux_Config((u8)self->pin, PINMUX_FUNCTION_GPIO);
        }
        self->initialized = false;
    }
}
