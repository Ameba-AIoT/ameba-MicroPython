/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Realtek Semiconductor Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <stddef.h>
#include "py/runtime.h"
#include "py/mphal.h"
#include "extmod/modmachine.h"
#include "extmod/virtpin.h"
#include "machine_pin.h"
#include "gpio_api.h"
#include "gpio_irq_api.h"
#include "PinNames.h"

// Forward declarations so the static table initializer can reference these types.
extern const mp_obj_type_t machine_pin_type;
extern const mp_obj_type_t machine_pin_irq_type;

// ---------------------------------------------------------------------------
// Pin mode constants
// ---------------------------------------------------------------------------
#define MP_PIN_MODE_IN         (0)
#define MP_PIN_MODE_OUT        (1)
#define MP_PIN_MODE_OPEN_DRAIN (2)

// ---------------------------------------------------------------------------
// Object structs
// ---------------------------------------------------------------------------

typedef struct _machine_pin_irq_obj_t {
    mp_obj_base_t base;
} machine_pin_irq_obj_t;

typedef struct _machine_pin_obj_t {
    mp_obj_base_t      base;
    PinName            id;
    uint8_t            mode;
    uint8_t            pull;
    uint8_t            irq_trigger;
    bool               gpio_configured;
    bool               irq_initialized;
    bool               od_output_high;
    gpio_t             gpio;
    gpio_irq_t         gpio_irq;
    machine_pin_irq_obj_t irq;
} machine_pin_obj_t;

// Recover the parent pin object from a pointer to its embedded irq member.
#define PIN_OBJ_FROM_IRQ(irq_ptr) \
    ((machine_pin_obj_t *)((char *)(irq_ptr) - offsetof(machine_pin_obj_t, irq)))

// ---------------------------------------------------------------------------
// Valid PinName check
// PA_0=0..PA_24=24, PB_0=32..PB_24=56
// ---------------------------------------------------------------------------
#define IS_VALID_PINNAME(p) \
    (((p) >= PA_0 && (p) <= PA_24) || ((p) >= PB_0 && (p) <= PB_24))

// ---------------------------------------------------------------------------
// Static singleton table
// ---------------------------------------------------------------------------

// Macro to fill one table slot.  All fields must be initialised so that
// zero-init gap entries (indices 25..31) remain clearly invalid (base.type==NULL).
#define AMEBA_PIN_ENTRY(PINNAME) \
    [PINNAME] = { \
        .base            = {.type = &machine_pin_type}, \
        .id              = (PINNAME), \
        .mode            = MP_PIN_MODE_IN, \
        .pull            = PullNone, \
        .irq_trigger     = 0, \
        .gpio_configured = false, \
        .irq_initialized = false, \
        .od_output_high  = true, \
        .gpio            = {.pin = (PINNAME)}, \
        .gpio_irq        = {.pin = (PINNAME)}, \
        .irq             = {.base = {.type = &machine_pin_irq_type}}, \
    }

// Array covers indices 0..56 (PA_0..PA_24 + gap 25..31 + PB_0..PB_24).
static machine_pin_obj_t machine_pin_obj_table[57] = {
    AMEBA_PIN_ENTRY(PA_0),
    AMEBA_PIN_ENTRY(PA_1),
    AMEBA_PIN_ENTRY(PA_2),
    AMEBA_PIN_ENTRY(PA_3),
    AMEBA_PIN_ENTRY(PA_4),
    AMEBA_PIN_ENTRY(PA_5),
    AMEBA_PIN_ENTRY(PA_6),
    AMEBA_PIN_ENTRY(PA_7),
    AMEBA_PIN_ENTRY(PA_8),
    AMEBA_PIN_ENTRY(PA_9),
    AMEBA_PIN_ENTRY(PA_10),
    AMEBA_PIN_ENTRY(PA_11),
    AMEBA_PIN_ENTRY(PA_12),
    AMEBA_PIN_ENTRY(PA_13),
    AMEBA_PIN_ENTRY(PA_14),
    AMEBA_PIN_ENTRY(PA_15),
    AMEBA_PIN_ENTRY(PA_16),
    AMEBA_PIN_ENTRY(PA_17),
    AMEBA_PIN_ENTRY(PA_18),
    AMEBA_PIN_ENTRY(PA_19),
    AMEBA_PIN_ENTRY(PA_20),
    AMEBA_PIN_ENTRY(PA_21),
    AMEBA_PIN_ENTRY(PA_22),
    AMEBA_PIN_ENTRY(PA_23),
    AMEBA_PIN_ENTRY(PA_24),
    // indices 25..31 are zero-initialised (invalid gap)
    AMEBA_PIN_ENTRY(PB_0),
    AMEBA_PIN_ENTRY(PB_1),
    AMEBA_PIN_ENTRY(PB_2),
    AMEBA_PIN_ENTRY(PB_3),
    AMEBA_PIN_ENTRY(PB_4),
    AMEBA_PIN_ENTRY(PB_5),
    AMEBA_PIN_ENTRY(PB_6),
    AMEBA_PIN_ENTRY(PB_7),
    AMEBA_PIN_ENTRY(PB_8),
    AMEBA_PIN_ENTRY(PB_9),
    AMEBA_PIN_ENTRY(PB_10),
    AMEBA_PIN_ENTRY(PB_11),
    AMEBA_PIN_ENTRY(PB_12),
    AMEBA_PIN_ENTRY(PB_13),
    AMEBA_PIN_ENTRY(PB_14),
    AMEBA_PIN_ENTRY(PB_15),
    AMEBA_PIN_ENTRY(PB_16),
    AMEBA_PIN_ENTRY(PB_17),
    AMEBA_PIN_ENTRY(PB_18),
    AMEBA_PIN_ENTRY(PB_19),
    AMEBA_PIN_ENTRY(PB_20),
    AMEBA_PIN_ENTRY(PB_21),
    AMEBA_PIN_ENTRY(PB_22),
    AMEBA_PIN_ENTRY(PB_23),
    AMEBA_PIN_ENTRY(PB_24),
};

// ---------------------------------------------------------------------------
// Board pins dict (empty in skeleton; Task 3 will populate)
// ---------------------------------------------------------------------------
static const mp_rom_map_elem_t machine_pin_board_pins_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_PA0),  MP_ROM_PTR(&machine_pin_obj_table[PA_0]) },
    { MP_ROM_QSTR(MP_QSTR_PA1),  MP_ROM_PTR(&machine_pin_obj_table[PA_1]) },
    { MP_ROM_QSTR(MP_QSTR_PA2),  MP_ROM_PTR(&machine_pin_obj_table[PA_2]) },
    { MP_ROM_QSTR(MP_QSTR_PA3),  MP_ROM_PTR(&machine_pin_obj_table[PA_3]) },
    { MP_ROM_QSTR(MP_QSTR_PA4),  MP_ROM_PTR(&machine_pin_obj_table[PA_4]) },
    { MP_ROM_QSTR(MP_QSTR_PA5),  MP_ROM_PTR(&machine_pin_obj_table[PA_5]) },
    { MP_ROM_QSTR(MP_QSTR_PA6),  MP_ROM_PTR(&machine_pin_obj_table[PA_6]) },
    { MP_ROM_QSTR(MP_QSTR_PA7),  MP_ROM_PTR(&machine_pin_obj_table[PA_7]) },
    { MP_ROM_QSTR(MP_QSTR_PA8),  MP_ROM_PTR(&machine_pin_obj_table[PA_8]) },
    { MP_ROM_QSTR(MP_QSTR_PA9),  MP_ROM_PTR(&machine_pin_obj_table[PA_9]) },
    { MP_ROM_QSTR(MP_QSTR_PA10), MP_ROM_PTR(&machine_pin_obj_table[PA_10]) },
    { MP_ROM_QSTR(MP_QSTR_PA11), MP_ROM_PTR(&machine_pin_obj_table[PA_11]) },
    { MP_ROM_QSTR(MP_QSTR_PA12), MP_ROM_PTR(&machine_pin_obj_table[PA_12]) },
    { MP_ROM_QSTR(MP_QSTR_PA13), MP_ROM_PTR(&machine_pin_obj_table[PA_13]) },
    { MP_ROM_QSTR(MP_QSTR_PA14), MP_ROM_PTR(&machine_pin_obj_table[PA_14]) },
    { MP_ROM_QSTR(MP_QSTR_PA15), MP_ROM_PTR(&machine_pin_obj_table[PA_15]) },
    { MP_ROM_QSTR(MP_QSTR_PA16), MP_ROM_PTR(&machine_pin_obj_table[PA_16]) },
    { MP_ROM_QSTR(MP_QSTR_PA17), MP_ROM_PTR(&machine_pin_obj_table[PA_17]) },
    { MP_ROM_QSTR(MP_QSTR_PA18), MP_ROM_PTR(&machine_pin_obj_table[PA_18]) },
    { MP_ROM_QSTR(MP_QSTR_PA19), MP_ROM_PTR(&machine_pin_obj_table[PA_19]) },
    { MP_ROM_QSTR(MP_QSTR_PA20), MP_ROM_PTR(&machine_pin_obj_table[PA_20]) },
    { MP_ROM_QSTR(MP_QSTR_PA21), MP_ROM_PTR(&machine_pin_obj_table[PA_21]) },
    { MP_ROM_QSTR(MP_QSTR_PA22), MP_ROM_PTR(&machine_pin_obj_table[PA_22]) },
    { MP_ROM_QSTR(MP_QSTR_PA23), MP_ROM_PTR(&machine_pin_obj_table[PA_23]) },
    { MP_ROM_QSTR(MP_QSTR_PA24), MP_ROM_PTR(&machine_pin_obj_table[PA_24]) },
    { MP_ROM_QSTR(MP_QSTR_PB0),  MP_ROM_PTR(&machine_pin_obj_table[PB_0]) },
    { MP_ROM_QSTR(MP_QSTR_PB1),  MP_ROM_PTR(&machine_pin_obj_table[PB_1]) },
    { MP_ROM_QSTR(MP_QSTR_PB2),  MP_ROM_PTR(&machine_pin_obj_table[PB_2]) },
    { MP_ROM_QSTR(MP_QSTR_PB3),  MP_ROM_PTR(&machine_pin_obj_table[PB_3]) },
    { MP_ROM_QSTR(MP_QSTR_PB4),  MP_ROM_PTR(&machine_pin_obj_table[PB_4]) },
    { MP_ROM_QSTR(MP_QSTR_PB5),  MP_ROM_PTR(&machine_pin_obj_table[PB_5]) },
    { MP_ROM_QSTR(MP_QSTR_PB6),  MP_ROM_PTR(&machine_pin_obj_table[PB_6]) },
    { MP_ROM_QSTR(MP_QSTR_PB7),  MP_ROM_PTR(&machine_pin_obj_table[PB_7]) },
    { MP_ROM_QSTR(MP_QSTR_PB8),  MP_ROM_PTR(&machine_pin_obj_table[PB_8]) },
    { MP_ROM_QSTR(MP_QSTR_PB9),  MP_ROM_PTR(&machine_pin_obj_table[PB_9]) },
    { MP_ROM_QSTR(MP_QSTR_PB10), MP_ROM_PTR(&machine_pin_obj_table[PB_10]) },
    { MP_ROM_QSTR(MP_QSTR_PB11), MP_ROM_PTR(&machine_pin_obj_table[PB_11]) },
    { MP_ROM_QSTR(MP_QSTR_PB12), MP_ROM_PTR(&machine_pin_obj_table[PB_12]) },
    { MP_ROM_QSTR(MP_QSTR_PB13), MP_ROM_PTR(&machine_pin_obj_table[PB_13]) },
    { MP_ROM_QSTR(MP_QSTR_PB14), MP_ROM_PTR(&machine_pin_obj_table[PB_14]) },
    { MP_ROM_QSTR(MP_QSTR_PB15), MP_ROM_PTR(&machine_pin_obj_table[PB_15]) },
    { MP_ROM_QSTR(MP_QSTR_PB16), MP_ROM_PTR(&machine_pin_obj_table[PB_16]) },
    { MP_ROM_QSTR(MP_QSTR_PB17), MP_ROM_PTR(&machine_pin_obj_table[PB_17]) },
    { MP_ROM_QSTR(MP_QSTR_PB18), MP_ROM_PTR(&machine_pin_obj_table[PB_18]) },
    { MP_ROM_QSTR(MP_QSTR_PB19), MP_ROM_PTR(&machine_pin_obj_table[PB_19]) },
    { MP_ROM_QSTR(MP_QSTR_PB20), MP_ROM_PTR(&machine_pin_obj_table[PB_20]) },
    { MP_ROM_QSTR(MP_QSTR_PB21), MP_ROM_PTR(&machine_pin_obj_table[PB_21]) },
    { MP_ROM_QSTR(MP_QSTR_PB22), MP_ROM_PTR(&machine_pin_obj_table[PB_22]) },
    { MP_ROM_QSTR(MP_QSTR_PB23), MP_ROM_PTR(&machine_pin_obj_table[PB_23]) },
    { MP_ROM_QSTR(MP_QSTR_PB24), MP_ROM_PTR(&machine_pin_obj_table[PB_24]) },
};
static MP_DEFINE_CONST_DICT(machine_pin_board_pins_locals_dict,
    machine_pin_board_pins_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    machine_pin_board_pins_obj_type,
    MP_QSTR_board,
    MP_TYPE_FLAG_NONE,
    locals_dict, &machine_pin_board_pins_locals_dict
    );

typedef struct {
    mp_obj_base_t base;
} machine_pin_board_obj_t;

static const machine_pin_board_obj_t machine_pin_board_obj = {
    { &machine_pin_board_pins_obj_type }
};

// ---------------------------------------------------------------------------
// machine_pin_find
// ---------------------------------------------------------------------------
static machine_pin_obj_t *machine_pin_find(mp_obj_t pin_in) {
    if (mp_obj_is_int(pin_in)) {
        int val = mp_obj_get_int(pin_in);
        if (IS_VALID_PINNAME(val)) {
            return &machine_pin_obj_table[val];
        }
    } else if (mp_obj_is_str(pin_in)) {
        const mp_map_t *m = &machine_pin_board_pins_locals_dict.map;
        mp_map_elem_t *elem = mp_map_lookup((mp_map_t *)m, pin_in, MP_MAP_LOOKUP);
        if (elem != NULL) {
            return MP_OBJ_TO_PTR(elem->value);
        }
    }
    mp_raise_ValueError(MP_ERROR_TEXT("invalid pin"));
}

// ---------------------------------------------------------------------------
// machine_pin_print
// ---------------------------------------------------------------------------
static void machine_pin_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *port = (self->id < PB_0) ? "PA" : "PB";
    int num = (self->id < PB_0) ? (int)self->id : (int)(self->id - PB_0);
    mp_printf(print, "Pin(%s%d)", port, num);
}

// ---------------------------------------------------------------------------
// machine_pin_init_helper — configure mode / pull / value
// ---------------------------------------------------------------------------
static mp_obj_t machine_pin_init_helper(machine_pin_obj_t *self, size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_mode, ARG_pull, ARG_value };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_mode,  MP_ARG_OBJ,                       {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_pull,  MP_ARG_OBJ,                       {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_value, MP_ARG_KW_ONLY | MP_ARG_OBJ,     {.u_obj = MP_OBJ_NULL} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_mode].u_obj != MP_OBJ_NULL) {
        int mode = mp_obj_get_int(args[ARG_mode].u_obj);
        self->mode = mode;
        if (mode == MP_PIN_MODE_IN) {
            gpio_dir(&self->gpio, PIN_INPUT);
        } else {
            gpio_dir(&self->gpio, PIN_OUTPUT);
            if (mode == MP_PIN_MODE_OPEN_DRAIN) {
                gpio_dir(&self->gpio, PIN_INPUT);  // default Hi-Z
                self->od_output_high = true;
            }
        }
    }

    if (args[ARG_pull].u_obj != MP_OBJ_NULL) {
        int pull = mp_obj_get_int(args[ARG_pull].u_obj);
        self->pull = pull;
        gpio_pull_ctrl(&self->gpio, pull);
    }

    if (args[ARG_value].u_obj != MP_OBJ_NULL) {
        int val = mp_obj_is_true(args[ARG_value].u_obj);
        if (self->mode == MP_PIN_MODE_OPEN_DRAIN) {
            if (val) {
                gpio_dir(&self->gpio, PIN_INPUT);
            } else {
                gpio_dir(&self->gpio, PIN_OUTPUT);
                gpio_write(&self->gpio, 0);
            }
            self->od_output_high = (bool)val;
        } else if (self->mode == MP_PIN_MODE_OUT) {
            gpio_write(&self->gpio, val);
        }
    }

    return mp_const_none;
}

static mp_obj_t machine_pin_obj_init(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    return machine_pin_init_helper(MP_OBJ_TO_PTR(args[0]), n_args - 1, args + 1, kw_args);
}
MP_DEFINE_CONST_FUN_OBJ_KW(machine_pin_init_obj, 1, machine_pin_obj_init);

// ---------------------------------------------------------------------------
// mp_pin_make_new
// ---------------------------------------------------------------------------
mp_obj_t mp_pin_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    (void)type;
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);

    machine_pin_obj_t *self = machine_pin_find(args[0]);

    if (!self->gpio_configured) {
        gpio_init(&self->gpio, self->id);
        self->gpio_configured = true;
    }

    if (n_args > 1 || n_kw > 0) {
        // Additional args → delegate to init_helper.
        mp_map_t kw_args_map;
        mp_map_init_fixed_table(&kw_args_map, n_kw, args + n_args);
        machine_pin_init_helper(self, n_args - 1, args + 1, &kw_args_map);
    }

    return MP_OBJ_FROM_PTR(self);
}

// ---------------------------------------------------------------------------
// machine_pin_call / value / on / off / toggle
// ---------------------------------------------------------------------------

static mp_obj_t machine_pin_call(mp_obj_t self_in, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 1, false);
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (n_args == 0) {
        return MP_OBJ_NEW_SMALL_INT(gpio_read(&self->gpio));
    }
    int val = mp_obj_is_true(args[0]);
    if (self->mode == MP_PIN_MODE_OPEN_DRAIN) {
        if (val) {
            gpio_dir(&self->gpio, PIN_INPUT);
        } else {
            gpio_dir(&self->gpio, PIN_OUTPUT);
            gpio_write(&self->gpio, 0);
        }
        self->od_output_high = (bool)val;
    } else {
        gpio_write(&self->gpio, val);
    }
    return mp_const_none;
}

static mp_obj_t machine_pin_value(size_t n_args, const mp_obj_t *args) {
    return machine_pin_call(args[0], n_args - 1, 0, args + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_pin_value_obj, 1, 2, machine_pin_value);

static mp_obj_t machine_pin_on(mp_obj_t self_in) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->mode == MP_PIN_MODE_OPEN_DRAIN) {
        gpio_dir(&self->gpio, PIN_INPUT);
        self->od_output_high = true;
    } else {
        gpio_direct_write(&self->gpio, true);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_on_obj, machine_pin_on);

static mp_obj_t machine_pin_off(mp_obj_t self_in) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->mode == MP_PIN_MODE_OPEN_DRAIN) {
        gpio_dir(&self->gpio, PIN_OUTPUT);
        gpio_write(&self->gpio, 0);
        self->od_output_high = false;
    } else {
        gpio_direct_write(&self->gpio, false);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_off_obj, machine_pin_off);

static mp_obj_t machine_pin_toggle(mp_obj_t self_in) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->mode == MP_PIN_MODE_OPEN_DRAIN) {
        if (self->od_output_high) {
            gpio_dir(&self->gpio, PIN_OUTPUT);
            gpio_write(&self->gpio, 0);
            self->od_output_high = false;
        } else {
            gpio_dir(&self->gpio, PIN_INPUT);
            self->od_output_high = true;
        }
    } else {
        gpio_write(&self->gpio, 1 - gpio_read(&self->gpio));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_toggle_obj, machine_pin_toggle);

// ---------------------------------------------------------------------------
// IRQ ISR — runs in interrupt context; only mp_sched_schedule is safe
// ---------------------------------------------------------------------------
static void machine_pin_isr_handler(uint32_t id, uint32_t event) {
    machine_pin_obj_t *self = (machine_pin_obj_t *)(uintptr_t)id;
    mp_obj_t handler = MP_STATE_PORT(machine_pin_irq_handler)[self->id];
    if (handler != MP_OBJ_NULL && handler != mp_const_none) {
        mp_sched_schedule(handler, MP_OBJ_FROM_PTR(self));
    }
}

// ---------------------------------------------------------------------------
// pin.irq(handler=None, trigger=IRQ_RISING_FALLING)
// ---------------------------------------------------------------------------
static mp_obj_t machine_pin_irq(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_handler, ARG_trigger };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_handler, MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_trigger, MP_ARG_INT, {.u_int = IRQ_FALL_RISE} },
    };
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (n_args > 1 || kw_args->used != 0) {
        mp_obj_t handler = args[ARG_handler].u_obj;
        uint32_t trigger = (uint32_t)args[ARG_trigger].u_int;
        MP_STATE_PORT(machine_pin_irq_handler)[self->id] = handler;
        self->irq_trigger = (uint8_t)trigger;

        if (handler == mp_const_none) {
            if (self->irq_initialized) {
                gpio_irq_disable(&self->gpio_irq);
            }
        } else {
            if (!self->irq_initialized) {
                gpio_irq_init(&self->gpio_irq, self->id,
                    machine_pin_isr_handler, (uint32_t)(uintptr_t)self);
                // gpio_irq_init() leaves GPIO_PuPd uninitialised in its stack
                // GPIO_InitTypeDef, clobbering any pull set at construction.
                // Re-apply the configured pull (see findings.md [P3]).
                gpio_irq_pull_ctrl(&self->gpio_irq, (PinMode)self->pull);
                self->irq_initialized = true;
            }
            gpio_irq_set_event(&self->gpio_irq, (gpio_irq_event)trigger);
            gpio_irq_enable(&self->gpio_irq);
        }
    }

    return MP_OBJ_FROM_PTR(&self->irq);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_pin_irq_obj, 1, machine_pin_irq);

// ---------------------------------------------------------------------------
// virtpin protocol
// ---------------------------------------------------------------------------
static mp_uint_t machine_pin_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    (void)errcode;
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    switch (request) {
        case MP_PIN_READ:
            return (mp_uint_t)gpio_read(&self->gpio);
        case MP_PIN_WRITE: {
            int val = (int)arg;
            if (self->mode == MP_PIN_MODE_OPEN_DRAIN) {
                if (val) {
                    gpio_dir(&self->gpio, PIN_INPUT);
                } else {
                    gpio_dir(&self->gpio, PIN_OUTPUT);
                    gpio_write(&self->gpio, 0);
                }
                self->od_output_high = (bool)val;
            } else {
                gpio_write(&self->gpio, val);
            }
            return 0;
        }
    }
    return (mp_uint_t)-1;
}

static const mp_pin_p_t machine_pin_pin_p = {
    .ioctl = machine_pin_ioctl,
};

// ---------------------------------------------------------------------------
// IRQ sub-type — irq.trigger([new]) + irq() soft-trigger
// ---------------------------------------------------------------------------
static mp_obj_t machine_pin_irq_trigger(size_t n_args, const mp_obj_t *args) {
    machine_pin_irq_obj_t *irq = MP_OBJ_TO_PTR(args[0]);
    machine_pin_obj_t *pin = PIN_OBJ_FROM_IRQ(irq);
    uint32_t orig = pin->irq_trigger;
    if (n_args == 2) {
        uint32_t new_trigger = (uint32_t)mp_obj_get_int(args[1]);
        pin->irq_trigger = (uint8_t)new_trigger;
        if (pin->irq_initialized) {
            gpio_irq_set_event(&pin->gpio_irq, (gpio_irq_event)new_trigger);
        }
    }
    return MP_OBJ_NEW_SMALL_INT(orig);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_pin_irq_trigger_obj, 1, 2,
    machine_pin_irq_trigger);

static mp_obj_t machine_pin_irq_call(mp_obj_t self_in, size_t n_args, size_t n_kw,
    const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    machine_pin_irq_obj_t *irq = MP_OBJ_TO_PTR(self_in);
    machine_pin_obj_t *pin = PIN_OBJ_FROM_IRQ(irq);
    machine_pin_isr_handler((uint32_t)(uintptr_t)pin, (gpio_irq_event)pin->irq_trigger);
    return mp_const_none;
}

static const mp_rom_map_elem_t machine_pin_irq_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_trigger), MP_ROM_PTR(&machine_pin_irq_trigger_obj) },
};
static MP_DEFINE_CONST_DICT(machine_pin_irq_locals_dict, machine_pin_irq_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_pin_irq_type,
    MP_QSTR_PinIRQ,
    MP_TYPE_FLAG_NONE,
    call, machine_pin_irq_call,
    locals_dict, &machine_pin_irq_locals_dict
    );

// ---------------------------------------------------------------------------
// Pin locals dict
// ---------------------------------------------------------------------------
static const mp_rom_map_elem_t machine_pin_locals_dict_table[] = {
    // Methods
    { MP_ROM_QSTR(MP_QSTR_init),        MP_ROM_PTR(&machine_pin_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_value),       MP_ROM_PTR(&machine_pin_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_on),          MP_ROM_PTR(&machine_pin_on_obj) },
    { MP_ROM_QSTR(MP_QSTR_off),         MP_ROM_PTR(&machine_pin_off_obj) },
    { MP_ROM_QSTR(MP_QSTR_toggle),      MP_ROM_PTR(&machine_pin_toggle_obj) },
    { MP_ROM_QSTR(MP_QSTR_irq),         MP_ROM_PTR(&machine_pin_irq_obj) },
    // Direction constants
    { MP_ROM_QSTR(MP_QSTR_IN),          MP_ROM_INT(MP_PIN_MODE_IN) },
    { MP_ROM_QSTR(MP_QSTR_OUT),         MP_ROM_INT(MP_PIN_MODE_OUT) },
    { MP_ROM_QSTR(MP_QSTR_OPEN_DRAIN),  MP_ROM_INT(MP_PIN_MODE_OPEN_DRAIN) },
    // Pull constants
    { MP_ROM_QSTR(MP_QSTR_PULL_NONE),   MP_ROM_INT(PullNone) },
    { MP_ROM_QSTR(MP_QSTR_PULL_UP),     MP_ROM_INT(PullUp) },
    { MP_ROM_QSTR(MP_QSTR_PULL_DOWN),   MP_ROM_INT(PullDown) },
    // IRQ trigger constants
    { MP_ROM_QSTR(MP_QSTR_IRQ_RISING),         MP_ROM_INT(IRQ_RISE) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_FALLING),        MP_ROM_INT(IRQ_FALL) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_RISING_FALLING), MP_ROM_INT(IRQ_FALL_RISE) },
    // Board pin namespace
    { MP_ROM_QSTR(MP_QSTR_board),       MP_ROM_PTR(&machine_pin_board_obj) },
};
static MP_DEFINE_CONST_DICT(machine_pin_locals_dict, machine_pin_locals_dict_table);

// ---------------------------------------------------------------------------
// machine_pin_type definition
// ---------------------------------------------------------------------------
MP_DEFINE_CONST_OBJ_TYPE(
    machine_pin_type,
    MP_QSTR_Pin,
    MP_TYPE_FLAG_NONE,
    make_new, mp_pin_make_new,
    print, machine_pin_print,
    call, machine_pin_call,
    protocol, &machine_pin_pin_p,
    locals_dict, &machine_pin_locals_dict
    );

// ---------------------------------------------------------------------------
// Root pointer: IRQ handler table (one slot per valid PinName index)
// ---------------------------------------------------------------------------
MP_REGISTER_ROOT_POINTER(mp_obj_t machine_pin_irq_handler[57]);
