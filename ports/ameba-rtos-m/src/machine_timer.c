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

#include <stdint.h>
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/mperrno.h"
#include "extmod/modmachine.h"
#include "shared/runtime/mpirq.h"
#include "machine_timer.h"
#include "timer_api.h"

#define TIMER_MODE_ONE_SHOT (0)
#define TIMER_MODE_PERIODIC (1)

// gtimer 的 32.768kHz 档（TIMER0..7）周期 = duration_us/1e6*32768 - 1，
// duration_us < ~61us 会下溢成超长周期。钳到 62us 保证所有档位都得到有效 reload 值。
// 软调度回调的实际延迟远大于 62us，此下限无实际损失。
#define MACHINE_TIMER_MIN_PERIOD_US (62)

// gtimer count: TIMER0..TIMER11. Pool excludes TIMER8 (PWM) and TIMER9 (SDK
// reserved); prefer the 1us timers (10/11) over the 32.768kHz ones (0..7).
// 数组按硬件 timer id（0..11）直接寻址，故大小固定为 12；槽 8/9 永不使用
// （已从 machine_timer_pool 排除），但保留以保持 id 与下标一一对应。
#define MACHINE_TIMER_COUNT (12)
#if defined(CONFIG_AMEBADPLUS)
static const uint8_t machine_timer_pool[] = {10, 11, 0, 1, 2, 3, 4, 5, 6, 7};
#else
// AmebaGreen2's GTIMER_MAX is only 9 (ids 0..8, TIMER8 reserved for PWM), and
// its gtimer_reload() -- called by every gtimer_start_one_shout/periodical --
// hardcodes assert_param(obj->timer_id < 4), tighter than GTIMER_MAX itself.
// Picking id 10/11 (Dplus's preferred ids, which don't exist here) indexes
// Green2's TIMx[]/APBPeriph_TIMx[] tables out of bounds, feeding a garbage
// pointer into RTIM_TimeBaseInit() -- this silently hung the board with no
// crash output at all (confirmed on hardware). Only 0..3 are valid here.
static const uint8_t machine_timer_pool[] = {0, 1, 2, 3};
#endif

typedef struct _machine_timer_obj_t {
    mp_obj_base_t base;
    gtimer_t gtimer;     // embedded HAL object; the ISR holds a pointer to it
    int8_t hw_id;        // allocated hardware timer id, -1 when not allocated
    int32_t virtual_id;  // logical id passed from Python, for repr only
    uint8_t mode;        // TIMER_MODE_ONE_SHOT / TIMER_MODE_PERIODIC
    uint32_t period_us;
    mp_obj_t callback;
    bool hard;           // run callback directly in the ISR instead of scheduling it
} machine_timer_obj_t;

// Pick the first free hardware timer from the preference pool; -1 if none.
static int8_t machine_timer_alloc(void) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(machine_timer_pool); i++) {
        uint8_t id = machine_timer_pool[i];
        if (MP_STATE_PORT(machine_timer_obj)[id] == NULL) {
            return (int8_t)id;
        }
    }
    return -1;
}

// Runs in interrupt context; id is the (uintptr_t)self we passed as hid.
static void machine_timer_isr(uint32_t id) {
    machine_timer_obj_t *self = (machine_timer_obj_t *)(uintptr_t)id;
    mp_irq_dispatch(self->callback, MP_OBJ_FROM_PTR(self), self->hard);
}

static void machine_timer_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    machine_timer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    qstr mode = self->mode == TIMER_MODE_ONE_SHOT ? MP_QSTR_ONE_SHOT : MP_QSTR_PERIODIC;
    mp_printf(print, "Timer(%d, mode=%q, period=%uus)", (int)self->virtual_id, mode, self->period_us);
}

static mp_obj_t machine_timer_init_helper(machine_timer_obj_t *self,
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_mode, ARG_callback, ARG_period, ARG_tick_hz, ARG_freq, ARG_hard };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_mode,     MP_ARG_KW_ONLY | MP_ARG_INT,  {.u_int = TIMER_MODE_PERIODIC} },
        { MP_QSTR_callback, MP_ARG_KW_ONLY | MP_ARG_OBJ,  {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_period,   MP_ARG_KW_ONLY | MP_ARG_INT,  {.u_int = 0xffffffff} },
        { MP_QSTR_tick_hz,  MP_ARG_KW_ONLY | MP_ARG_INT,  {.u_int = 1000} },
        { MP_QSTR_freq,     MP_ARG_KW_ONLY | MP_ARG_OBJ,  {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_hard,     MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    self->hard = args[ARG_hard].u_bool;

    // Compute period in microseconds.
    uint64_t period_us;
    if (args[ARG_freq].u_obj != mp_const_none) {
        mp_int_t freq = mp_obj_get_int(args[ARG_freq].u_obj);
        if (freq <= 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid freq"));
        }
        period_us = 1000000ULL / (uint64_t)freq;
    } else {
        if (args[ARG_tick_hz].u_int <= 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid tick_hz"));
        }
        if (args[ARG_period].u_int < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid period"));
        }
        period_us = (uint64_t)(uint32_t)args[ARG_period].u_int * 1000000ULL
            / (uint64_t)args[ARG_tick_hz].u_int;
    }
    if (period_us < MACHINE_TIMER_MIN_PERIOD_US) {
        period_us = MACHINE_TIMER_MIN_PERIOD_US;
    }
    if (period_us > 0xFFFFFFFFULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("period too long"));
    }

    // re-init 时先停掉旧硬件，再写字段，避免 stop 前的 pending 中断读到半更新状态。
    bool need_alloc = (self->hw_id < 0);
    if (!need_alloc) {
        gtimer_stop(&self->gtimer);
    }

    self->mode = args[ARG_mode].u_int;
    self->period_us = (uint32_t)period_us;
    self->callback = args[ARG_callback].u_obj;

    // Allocate a hardware timer on first init; reuse it on re-init.
    if (need_alloc) {
        int8_t id = machine_timer_alloc();
        if (id < 0) {
            mp_raise_OSError(MP_ENODEV);
        }
        self->hw_id = id;
        MP_STATE_PORT(machine_timer_obj)[id] = self;
        gtimer_init(&self->gtimer, (uint32_t)id);
    }

    uint32_t hid = (uint32_t)(uintptr_t)self;
    if (self->mode == TIMER_MODE_ONE_SHOT) {
        gtimer_start_one_shout(&self->gtimer, self->period_us, machine_timer_isr, hid);
    } else {
        gtimer_start_periodical(&self->gtimer, self->period_us, machine_timer_isr, hid);
    }
    return mp_const_none;
}

static mp_obj_t machine_timer_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *args) {
    machine_timer_obj_t *self = mp_obj_malloc_with_finaliser(machine_timer_obj_t, &machine_timer_type);
    self->hw_id = -1;
    self->callback = mp_const_none;
    self->mode = TIMER_MODE_PERIODIC;
    self->period_us = 0;
    self->hard = false;

    // First positional arg is the (virtual) timer id; defaults to -1.
    mp_int_t id = -1;
    if (n_args > 0) {
        id = mp_obj_get_int(args[0]);
        --n_args;
        ++args;
    }
    self->virtual_id = (int32_t)id;

    if (n_args > 0 || n_kw > 0) {
        mp_map_t kw_args;
        mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
        machine_timer_init_helper(self, n_args, args, &kw_args);
    }
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t machine_timer_init(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    return machine_timer_init_helper(MP_OBJ_TO_PTR(args[0]), n_args - 1, args + 1, kw_args);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_timer_init_obj, 1, machine_timer_init);

static mp_obj_t machine_timer_deinit(mp_obj_t self_in) {
    machine_timer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->hw_id >= 0) {
        gtimer_stop(&self->gtimer);
        gtimer_deinit(&self->gtimer);
        MP_STATE_PORT(machine_timer_obj)[self->hw_id] = NULL;
        self->hw_id = -1;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_timer_deinit_obj, machine_timer_deinit);

static const mp_rom_map_elem_t machine_timer_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&machine_timer_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_init),    MP_ROM_PTR(&machine_timer_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),  MP_ROM_PTR(&machine_timer_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_ONE_SHOT), MP_ROM_INT(TIMER_MODE_ONE_SHOT) },
    { MP_ROM_QSTR(MP_QSTR_PERIODIC), MP_ROM_INT(TIMER_MODE_PERIODIC) },
};
static MP_DEFINE_CONST_DICT(machine_timer_locals_dict, machine_timer_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_timer_type,
    MP_QSTR_Timer,
    MP_TYPE_FLAG_NONE,
    make_new, machine_timer_make_new,
    print, machine_timer_print,
    locals_dict, &machine_timer_locals_dict
    );

// Root the timer objects so their callbacks survive GC; the slot index is the
// hardware timer id, so a non-NULL slot also means that timer is in use.
MP_REGISTER_ROOT_POINTER(struct _machine_timer_obj_t *machine_timer_obj[MACHINE_TIMER_COUNT]);
