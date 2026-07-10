// SPDX-License-Identifier: MIT
// machine.PWM port implementation for ameba-rtos.
//
// INCLUDEFILE — included by extmod/machine_pwm.c via
// MICROPY_PY_MACHINE_PWM_INCLUDEFILE.  Defines machine_pwm_obj_t and all
// mp_machine_pwm_* functions required by the extmod framework.
//
// AmebaDplus:  8 channels share a single TIMER8 (40 MHz).  All channels run
//              at the SAME frequency; each has an independent duty cycle.
// AmebaGreen2: 8 channels spread across TIM4-TIM5 (4 channels each, 40 MHz).
//              Same shared-frequency constraint applies because the prescaler
//              global in the SDK's pwmout_api.c is not per-timer.
// The SDK's pwmout_init/pwmout_free manage pinmux and timer gating.

#include "pwmout_api.h"
#include "pwmout_ex_api.h"   // pwmout_start, pwmout_stop
#include "ameba_soc.h"       // Pinmux_Config, PINMUX_FUNCTION_GPIO
#if defined(CONFIG_AMEBAGREEN2)
#include "ameba_pwmtimer.h"  // TIM_CCInitTypeDef, RTIM_CCxInit, TIMx[]
#endif

// The SDK's pwmout_api.c owns a global `prescaler` (shared across all PWM
// channels / timers).  pwmout_period_us() bumps it when a long period
// overflows the 16-bit ARR, but never writes the hardware PSC register and
// never resets it for short periods.  We own the prescaler in
// mp_machine_pwm_freq_set() and keep global + hardware in lock-step.
extern u8 prescaler;

#if defined(CONFIG_AMEBAGREEN2)
// Green2 PWM uses TIM4-TIM7 (indices 4-7).  timer_start[] tracks which
// timers have been initialised; used here to know which need a prescaler
// hardware update when the shared prescaler changes.
#define GREEN2_PWM_TIMER_BASE  (4)
#define GREEN2_PWM_TIMER_COUNT (2)   // we map 8 channels across TIM4 and TIM5
extern u8 timer_start[];             // from SDK pwmout_api.c
#endif

#define PWM_CHANNEL_MAX    (8)
#define PWM_TIMER_CLK_MHZ  (40)

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

// Return the active channel already bound to this pin, or NULL.  Used to make
// a second PWM(pin) on the same pin reuse (reconfigure) the existing channel
// instead of allocating a new one — matching esp32's find_channel semantics.
static machine_pwm_obj_t *machine_pwm_find_by_pin(PinName pin) {
    for (int i = 0; i < PWM_CHANNEL_MAX; i++) {
        if (machine_pwm_obj[i].active && machine_pwm_obj[i].pin == pin) {
            return &machine_pwm_obj[i];
        }
    }
    return NULL;
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

    // Resolve the Pin object / pin name to a PinName.  mp_hal_pin_resolve
    // validates the type via machine_pin_find and raises a clean ValueError
    // on a non-pin argument, instead of blindly dereferencing it as a pointer
    // (undefined behaviour that can bind / drive a garbage pin).
    PinName pin = mp_hal_pin_resolve(args[0]);

    // Only pins PA6+ support PWM on AmebaDplus.
    if (pin < PA_6) {
        mp_raise_ValueError(MP_ERROR_TEXT("pin doesn't support PWM"));
    }

    // If this pin already drives an active channel, reuse it (esp32 semantics):
    // a second PWM(pin) reconfigures the existing channel via init_helper below
    // rather than raising or consuming a second channel slot.
    machine_pwm_obj_t *self = machine_pwm_find_by_pin(pin);
    if (self == NULL) {
        // Allocate a fresh channel for this pin.
        int channel = machine_pwm_alloc_channel();
        if (channel < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("all PWM channels in use"));
        }

        self = &machine_pwm_obj[channel];
        self->base.type = type;
        self->pwm_idx = (uint8_t)channel;
        self->pin = pin;

        // Init PWM: channel fields MUST be set before pwmout_init.
        // AmebaDplus:  single TIM8, pwm_idx = 0-7.
        // AmebaGreen2: TIM4 for channels 0-3, TIM5 for channels 4-7;
        //              pwmtimer_idx = timer number (4 or 5),
        //              pwm_idx      = channel within that timer (0-3).
        #if defined(CONFIG_AMEBAGREEN2)
        self->pwm.pwmtimer_idx = GREEN2_PWM_TIMER_BASE + self->pwm_idx / 4;
        self->pwm.pwm_idx      = self->pwm_idx % 4;
        #else
        self->pwm.pwm_idx = self->pwm_idx;
        #endif
        pwmout_init(&self->pwm, (PinName)self->pin);

        #if defined(CONFIG_AMEBAGREEN2)
        // pwmout_init does not call RTIM_CCxInit, so CCRx[n] may remain in
        // hardware-reset state (bit 27 = InputCapture mode).  The raw SDK
        // example (raw_pwm) always calls RTIM_CCxInit after RTIM_TimeBaseInit;
        // replicate that here so the channel is correctly configured as PWM
        // output before any duty-cycle writes.
        {
            TIM_CCInitTypeDef cc;
            RTIM_CCStructInit(&cc);
            cc.TIM_OCPulse = 0;
            RTIM_CCxInit(TIMx[self->pwm.pwmtimer_idx], &cc, self->pwm.pwm_idx);
            RTIM_CCxCmd(TIMx[self->pwm.pwmtimer_idx], self->pwm.pwm_idx, TIM_CCx_Enable);
        }
        #endif

        self->active = true;
    }

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

    // Compute and apply the prescaler ourselves *before* pwmout_period_us().
    // TIMER8 counts at 40 MHz into a 16-bit ARR; a period needing more than
    // 0x10000 ticks must be prescaled by the SDK's u8 `prescaler` global
    // (formula: period_us * 40 / 0x10000).  The SDK's pwmout_period_us() bumps
    // that global for long periods but never (a) writes it to the hardware PSC
    // register nor (b) resets it to 0 for short periods, so after the first
    // low-frequency call it stays stale and every later frequency is wrong.
    //
    // We instead own the prescaler here: derive it, clamp to the u8 hardware
    // limit, and push the SAME value into both the global and the hardware PSC
    // (immediate reload).  Keeping them in lock-step means pwmout_period_us()'s
    // own `if (tmp > 0x10000)` branch never fires (a no-op), so the ARR it
    // programs always matches the live prescaler.
    //
    // The u8 prescaler caps the longest period at (0x10000 * 256) / 40 MHz
    // ≈ 419 ms, i.e. a hardware floor of ~2.39 Hz.  Lower requests are clamped
    // to that floor rather than silently wrapping the u8 and corrupting output.
    uint32_t ticks = (uint32_t)new_period_us * PWM_TIMER_CLK_MHZ;
    uint32_t new_prescaler = (ticks > 0x10000) ? (ticks / 0x10000) : 0;
    if (new_prescaler > 0xFF) {
        new_prescaler = 0xFF;
        new_period_us = (int)((0x10000UL * (0xFF + 1)) / PWM_TIMER_CLK_MHZ);
    }
    if (new_prescaler != (uint32_t)prescaler) {
        prescaler = (u8)new_prescaler;
        // Push the new prescaler to every initialised PWM timer so that the
        // hardware and the `prescaler` global stay in lock-step.
        #if defined(CONFIG_AMEBAGREEN2)
        for (int ti = 0; ti < GREEN2_PWM_TIMER_COUNT; ti++) {
            if (timer_start[ti]) {
                RTIM_PrescalerConfig(TIMx[GREEN2_PWM_TIMER_BASE + ti],
                    new_prescaler, TIM_PSCReloadMode_Immediate);
            }
        }
        #else
        RTIM_PrescalerConfig(TIM8, new_prescaler, TIM_PSCReloadMode_Immediate);
        #endif
    }

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
