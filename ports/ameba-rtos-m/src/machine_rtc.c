// SPDX-License-Identifier: MIT
// machine.RTC for ameba-rtos (AmebaDplus).
//
// Standalone compilation (not INCLUDEFILE).  Registered in the machine
// module via MICROPY_PY_MACHINE_EXTRA_GLOBALS in modmachine.c.

#include <time.h>

#include "py/runtime.h"
#include "extmod/modmachine.h"
#include "shared/timeutils/timeutils.h"

#include "rtc_api.h"

typedef struct _machine_rtc_obj_t {
    mp_obj_base_t base;
} machine_rtc_obj_t;

static machine_rtc_obj_t machine_rtc_obj = {{&machine_rtc_type}};

static mp_obj_t machine_rtc_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    if (!rtc_isenabled()) {
        rtc_init();
    }
    return (mp_obj_t)&machine_rtc_obj;
}

static mp_obj_t machine_rtc_datetime(mp_uint_t n_args, const mp_obj_t *args) {
    if (n_args == 1) {
        // Get: read Unix timestamp from hardware, convert to struct_time, return tuple.
        time_t t = rtc_read();
        timeutils_struct_time_t tm;
        timeutils_seconds_since_epoch_to_struct_time(t, &tm);
        mp_obj_t tuple[8] = {
            mp_obj_new_int(tm.tm_year),
            mp_obj_new_int(tm.tm_mon),
            mp_obj_new_int(tm.tm_mday),
            mp_obj_new_int(tm.tm_wday),
            mp_obj_new_int(tm.tm_hour),
            mp_obj_new_int(tm.tm_min),
            mp_obj_new_int(tm.tm_sec),
            mp_obj_new_int(0), // subsecond
        };
        return mp_obj_new_tuple(8, tuple);
    } else {
        // Set: unpack tuple, convert to Unix timestamp, write to hardware.
        mp_obj_t *items;
        mp_obj_get_array_fixed_n(args[1], 8, &items);
        time_t t = (time_t)timeutils_seconds_since_epoch(
            mp_obj_get_int(items[0]), // year
            mp_obj_get_int(items[1]), // month
            mp_obj_get_int(items[2]), // day
            mp_obj_get_int(items[4]), // hour
            mp_obj_get_int(items[5]), // minute
            mp_obj_get_int(items[6])  // second
        );
        rtc_write(t);
        return mp_const_none;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_rtc_datetime_obj, 1, 2, machine_rtc_datetime);

static const mp_rom_map_elem_t machine_rtc_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_datetime), MP_ROM_PTR(&machine_rtc_datetime_obj) },
};
static MP_DEFINE_CONST_DICT(machine_rtc_locals_dict, machine_rtc_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_rtc_type,
    MP_QSTR_RTC,
    MP_TYPE_FLAG_NONE,
    make_new, machine_rtc_make_new,
    locals_dict, &machine_rtc_locals_dict
    );
