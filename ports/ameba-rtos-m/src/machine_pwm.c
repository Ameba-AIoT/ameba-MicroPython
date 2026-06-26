// SPDX-License-Identifier: MIT
// machine.PWM port implementation for ameba-rtos (AmebaDplus).
//
// INCLUDEFILE — included by extmod/machine_pwm.c via
// MICROPY_PY_MACHINE_PWM_INCLUDEFILE.  Defines machine_pwm_obj_t and all
// mp_machine_pwm_* functions required by the extmod framework.
//
// Hardware: 8 PWM channels share TIMER8, so ALL channels run at the SAME
// frequency.  Each channel has an independent pulse width (duty cycle).
// The SDK's pwmout_init/pwmout_free manage pinmux and timer gating.

#include "pwmout_api.h"
#include "pwmout_ex_api.h"   // pwmout_start, pwmout_stop
#include "ameba_soc.h"       // Pinmux_Config, PINMUX_FUNCTION_GPIO

// Declared in machine_pin.c, used to extract PinName from a Pin object.
extern mp_hal_pin_obj_t mp_hal_get_pin_obj(void *pin_obj);

#define PWM_CHANNEL_MAX    (8)

typedef struct _machine_pwm_obj_t {
    mp_obj_base_t base;
    pwmout_t pwm;          // mbed HAL object, embedded
    uint8_t  pwm_idx;      // 0..7
    PinName  pin;
    bool     active;       // channel allocated and initialised
} machine_pwm_obj_t;

static machine_pwm_obj_t machine_pwm_obj[PWM_CHANNEL_MAX];

// ---------------------------------------------------------------------------
// Channel allocator: first-fit
// ---------------------------------------------------------------------------
static int8_t machine_pwm_alloc_channel(void) {
    for (int i = 0; i < PWM_CHANNEL_MAX; i++) {
        if (!machine_pwm_obj[i].active) {
            return i;
        }
    }
    return -1;   // all channels in use
}

// Check whether a given pin is already claimed by an active PWM channel.
static bool machine_pwm_pin_is_claimed(PinName pin) {
    for (int i = 0; i < PWM_CHANNEL_MAX; i++) {
        if (machine_pwm_obj[i].active && machine_pwm_obj[i].pin == pin) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// print
// ---------------------------------------------------------------------------
static void mp_machine_pwm_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    machine_pwm_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *pp = (self->pin < PB_0) ? "PA" : "PB";
    int pn = (self->pin < PB_0) ? (int)self->pin : (int)(self->pin - PB_0);
    mp_printf(print, "PWM(%s%d, freq=%u, duty_u16=%u)",
        pp, pn,
        (self->pwm.period > 0) ? 1000000 / self->pwm.period : 0,
        (uint16_t)(pwmout_read(&self->pwm) * 65535.0f));
}

// ---------------------------------------------------------------------------
// init helper
// ---------------------------------------------------------------------------
static void mp_machine_pwm_init_helper(machine_pwm_obj_t *self,
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    enum { ARG_freq, ARG_duty_u16, ARG_duty_ns };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_freq,     MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_duty_u16, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_duty_ns,  MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_freq].u_int > 0) {
        mp_machine_pwm_freq_set(self, args[ARG_freq].u_int);
    }
    if (args[ARG_duty_u16].u_int >= 0) {
        mp_machine_pwm_duty_set_u16(self, args[ARG_duty_u16].u_int);
    } else if (args[ARG_duty_ns].u_int >= 0) {
        mp_machine_pwm_duty_set_ns(self, args[ARG_duty_ns].u_int);
    }
}

// ---------------------------------------------------------------------------
// make_new
// ---------------------------------------------------------------------------
static mp_obj_t mp_machine_pwm_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *args) {

    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);

    // Extract PinName from the Pin object.
    PinName pin = mp_hal_get_pin_obj(args[0]);

    // Only pins PA6+ support PWM on AmebaDplus.
    if (pin < PA_6) {
        mp_raise_ValueError(MP_ERROR_TEXT("pin doesn't support PWM"));
    }

    // Reject if the same pin is already claimed by another channel.
    if (machine_pwm_pin_is_claimed(pin)) {
        mp_raise_ValueError(MP_ERROR_TEXT("PWM pin already in use"));
    }

    // Find a free PWM channel.
    int channel = machine_pwm_alloc_channel();
    if (channel < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("all PWM channels in use"));
    }

    machine_pwm_obj_t *self = &machine_pwm_obj[channel];
    self->base.type = type;
    self->pwm_idx = (uint8_t)channel;
    self->pin = pin;

    // Init PWM: pwm_idx MUST be set before pwmout_init.
    self->pwm.pwm_idx = self->pwm_idx;
    pwmout_init(&self->pwm, (PinName)self->pin);
    self->active = true;

    // Process remaining kwargs (freq, duty_u16, duty_ns, invert).
    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
    mp_machine_pwm_init_helper(self, n_args - 1, args + 1, &kw_args);

    return MP_OBJ_FROM_PTR(self);
}

// ---------------------------------------------------------------------------
// deinit
// ---------------------------------------------------------------------------
static void mp_machine_pwm_deinit(machine_pwm_obj_t *self) {
    if (self->active) {
        pwmout_free(&self->pwm);
        Pinmux_Config((u8)self->pin, PINMUX_FUNCTION_GPIO);
        self->active = false;
    }
}

// ---------------------------------------------------------------------------
// freq
// ---------------------------------------------------------------------------
static mp_obj_t mp_machine_pwm_freq_get(machine_pwm_obj_t *self) {
    if (self->pwm.period > 0) {
        return MP_OBJ_NEW_SMALL_INT(1000000 / self->pwm.period);
    }
    return MP_OBJ_NEW_SMALL_INT(0);
}

static void mp_machine_pwm_freq_set(machine_pwm_obj_t *self, mp_int_t freq) {
    if (freq <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("freq must be positive"));
    }

    int new_period_us = 1000000 / freq;

    // All 8 PWM channels share TIMER8 (same period).  Changing the period
    // affects every active channel's duty cycle because the raw HW compare
    // value stays unchanged while the period (ARR) changes.
    //
    // pwmout_period_us() only re-applies the calling channel's duty fraction
    // under the new period.  Save every other active channel's duty fraction
    // beforehand so we can restore them afterwards.
    float saved_duty[PWM_CHANNEL_MAX];
    int n_saved = 0;
    for (int i = 0; i < PWM_CHANNEL_MAX; i++) {
        if (i != self->pwm_idx && machine_pwm_obj[i].active) {
            saved_duty[i] = pwmout_read(&machine_pwm_obj[i].pwm);
            n_saved++;
        }
    }

    // Change the shared period.  pwmout_period_us internally re-applies
    // self's duty fraction via pwmout_write().
    pwmout_period_us(&self->pwm, new_period_us);

    // Sync the bookkeeping period and re-apply duty for all other channels.
    // pwmout_write() uses obj->period to compute the raw compare value, so
    // we must update it first.
    if (n_saved > 0) {
        for (int i = 0; i < PWM_CHANNEL_MAX; i++) {
            if (i != self->pwm_idx && machine_pwm_obj[i].active) {
                machine_pwm_obj[i].pwm.period = (uint32_t)new_period_us;
                // pwmout_write computes obj->pulse = percent * obj->period
                // and then writes the raw CCR register.
                pwmout_write(&machine_pwm_obj[i].pwm, saved_duty[i]);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// duty_u16
// ---------------------------------------------------------------------------
static mp_obj_t mp_machine_pwm_duty_get_u16(machine_pwm_obj_t *self) {
    return MP_OBJ_NEW_SMALL_INT((mp_int_t)(pwmout_read(&self->pwm) * 65535.0f));
}

static void mp_machine_pwm_duty_set_u16(machine_pwm_obj_t *self, mp_int_t duty_u16) {
    if (duty_u16 < 0) {
        duty_u16 = 0;
    } else if (duty_u16 > 65535) {
        duty_u16 = 65535;
    }
    pwmout_write(&self->pwm, (float)duty_u16 / 65535.0f);
}

// ---------------------------------------------------------------------------
// duty_ns
// ---------------------------------------------------------------------------
static mp_obj_t mp_machine_pwm_duty_get_ns(machine_pwm_obj_t *self) {
    return MP_OBJ_NEW_SMALL_INT((mp_int_t)(self->pwm.pulse * 1000.0f));
}

static void mp_machine_pwm_duty_set_ns(machine_pwm_obj_t *self, mp_int_t duty_ns) {
    if (duty_ns < 0) {
        duty_ns = 0;
    }
    // Clamp to 100% duty (pulse width cannot exceed period).
    uint32_t max_ns = self->pwm.period * 1000;
    if ((uint32_t)duty_ns > max_ns) {
        duty_ns = (mp_int_t)max_ns;
    }
    pwmout_pulsewidth_us(&self->pwm, duty_ns / 1000);
}

// ---------------------------------------------------------------------------
// Soft-reset cleanup (called from mp_main.c)
// ---------------------------------------------------------------------------
void machine_pwm_deinit_all(void) {
    for (int i = 0; i < PWM_CHANNEL_MAX; i++) {
        machine_pwm_obj_t *self = &machine_pwm_obj[i];
        if (self->active) {
            pwmout_free(&self->pwm);
            Pinmux_Config((u8)self->pin, PINMUX_FUNCTION_GPIO);
            self->active = false;
        }
    }
}
