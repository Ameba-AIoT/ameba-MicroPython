// SPDX-License-Identifier: MIT
// machine.RTC for ameba-rtos (AmebaDplus).
//
// Standalone compilation (not INCLUDEFILE).  Registered in the machine
// module via MICROPY_PY_MACHINE_EXTRA_GLOBALS in modmachine.c.

#include <time.h>
#include <sys/time.h>

#include "py/runtime.h"
#include "extmod/modmachine.h"
#include "shared/runtime/mpirq.h"
#include "shared/timeutils/timeutils.h"

#include "modmachine.h" // MP_MACHINE_WAKE_IDLE/SLEEP/DEEPSLEEP (defined in modmachine.c's translation unit; exported here)
#include "rtc_api.h"

// _settimeofday() (implemented in mphalport.c, this port's own -- neither
// the toolchain's nosys stub nor any SDK component actually linked into
// this build provides a working one) isn't declared in any toolchain header
// (settimeofday() itself isn't part of this newlib's syscall table at all)
// -- see its use in machine_rtc_datetime() below. _gettimeofday() *is*
// declared in sys/time.h, but only under `#ifdef _COMPILING_NEWLIB`, so it
// needs a declaration here too -- see mbedtls_ms_time() below for why this
// port calls it directly instead of the public gettimeofday().
int _settimeofday(struct timeval *ptimeval, void *ptimezone);
int _gettimeofday(struct timeval *ptimeval, void *ptimezone);

// Convert a target Unix timestamp into the hardware's alarm_t format.
// alarm_t.yday is 0-based (Jan 1 = 0) -- confirmed via rtc_write()'s use of
// libc localtime()'s 0-based tm_yday feeding RTC_Days directly with no
// adjustment. timeutils_struct_time_t.tm_yday is 1-based, hence the -1.
static void rtc_alarm_target_from_timestamp(time_t target, alarm_t *out) {
    timeutils_struct_time_t tm;
    timeutils_seconds_since_epoch_to_struct_time(target, &tm);
    out->yday = tm.tm_yday - 1;
    out->hour = tm.tm_hour;
    out->min = tm.tm_min;
    out->sec = tm.tm_sec;
}

// time is a millisecond offset from now.
static void rtc_alarm_target_from_offset_ms(time_t now, mp_int_t offset_ms, alarm_t *out) {
    time_t target = now + (time_t)(offset_ms / 1000);
    rtc_alarm_target_from_timestamp(target, out);
}

// time is an 8-tuple datetimetuple: (year, month, day, weekday, hour, min, sec, subsec).
static void rtc_alarm_target_from_tuple(const mp_obj_t *items, alarm_t *out) {
    time_t target = (time_t)timeutils_seconds_since_epoch(
        mp_obj_get_int(items[0]), // year
        mp_obj_get_int(items[1]), // month
        mp_obj_get_int(items[2]), // day
        mp_obj_get_int(items[4]), // hour
        mp_obj_get_int(items[5]), // minute
        mp_obj_get_int(items[6])  // second
    );
    rtc_alarm_target_from_timestamp(target, out);
}

// Milliseconds remaining until target; 0 if already past.
static uint32_t rtc_alarm_ms_left(time_t now, time_t target) {
    if (target <= now) {
        return 0;
    }
    return (uint32_t)(target - now) * 1000;
}

typedef struct _machine_rtc_obj_t {
    mp_obj_base_t base;
    time_t alarm_target;       // Unix timestamp the alarm is armed for; 0 = no alarm armed
    bool alarm_repeat;         // true if alarm(time, repeat=True) was used (ms-offset mode only)
    mp_int_t alarm_interval_ms; // original interval, used to re-arm on each fire when alarm_repeat
} machine_rtc_obj_t;

static machine_rtc_obj_t machine_rtc_obj = {{&machine_rtc_type}, 0, false, 0};

// The irq object is heap-allocated (mp_irq_new()) but machine_rtc_obj is a
// static BSS singleton, not a GC root -- a raw pointer stored only on it
// would be invisible to the collector and get freed out from under a live
// alarm (use-after-free the next time the ISR fires). Root it the same way
// every other IRQ-capable peripheral in this port does (see e.g.
// modota.c's ota_active MP_REGISTER_ROOT_POINTER below), and reuse the
// existing object across repeated irq() calls instead of orphaning one each
// time (matches micropython/ports/mimxrt/machine_rtc.c's
// MP_STATE_PORT(machine_rtc_irq_object) pattern for the same
// single-instance-RTC situation).

// Single static scheduler node -- re-arming the alarm must be deferred out of
// ISR context (see machine_rtc_alarm_irq_handler below), and there is only
// ever one RTC alarm object in this port, so a single file-static node
// (mirroring machine_rtc_obj itself) is enough; no per-fire allocation needed.
static mp_sched_node_t machine_rtc_alarm_sched_node;

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
        // Mirror this write into the libc wall clock (matches esp32's
        // machine_rtc.c, modulo the underscore -- this SDK's newlib syscall
        // stub is named _settimeofday(), not settimeofday(); see gettod.c)
        // so mbedtls's MBEDTLS_HAVE_TIME_DATE certificate date checks --
        // which read time()/gettimeofday(), not the RTC peripheral directly
        // -- see a real time once this is set. Without this, setting
        // machine.RTC() (including via ntptime.settime(), which calls this
        // same setter) would have no effect on TLS.
        struct timeval tv = {.tv_sec = t, .tv_usec = 0};
        _settimeofday(&tv, NULL);
        return mp_const_none;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_rtc_datetime_obj, 1, 2, machine_rtc_datetime);

static void rtc_alarm_arm(machine_rtc_obj_t *self, alarm_irq_handler handler) {
    alarm_t a;
    rtc_alarm_target_from_timestamp(self->alarm_target, &a);
    rtc_set_alarm(&a, handler);
}

// Forward-declared; defined below. Handles repeat re-arm bookkeeping and
// dispatches to the registered mp_irq handler (see machine_rtc_irq()).
static void machine_rtc_alarm_irq_handler(void);

static mp_obj_t machine_rtc_alarm(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_id, ARG_time, ARG_repeat };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_id,     MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_time,   MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_repeat, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_id].u_int != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid alarm id"));
    }

    machine_rtc_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    time_t now = rtc_read();
    alarm_t a;

    if (mp_obj_is_int(args[ARG_time].u_obj)) {
        mp_int_t offset_ms = mp_obj_get_int(args[ARG_time].u_obj);
        // The hardware alarm_t has whole-second resolution only (yday/hour/
        // min/sec fields, no sub-second field -- see rtc_alarm_target_from_
        // timestamp above). machine_rtc_alarm_irq_handler()'s repeat re-arm
        // advances alarm_target by (alarm_interval_ms / 1000) seconds each
        // fire; for offset_ms < 1000 that division truncates to 0, so the
        // "re-armed" target never actually moves into the future. Confirmed
        // on hardware: the RTC alarm condition (target <= now) then stays
        // permanently true, so the vendor ISR re-fires continuously (observed
        // 600 fires within ~450ms) until the interrupt is somehow left
        // disabled -- not just imprecise, an unrecoverable storm-then-stuck
        // state. Reject rather than silently produce that: there's no
        // representable "sub-second repeat" on this hardware alarm.
        if (args[ARG_repeat].u_bool && offset_ms < 1000) {
            mp_raise_ValueError(MP_ERROR_TEXT("repeat alarm requires time >= 1000ms (hardware alarm has 1s resolution)"));
        }
        rtc_alarm_target_from_offset_ms(now, offset_ms, &a);
        self->alarm_target = now + (time_t)(offset_ms / 1000);
        self->alarm_repeat = args[ARG_repeat].u_bool;
        self->alarm_interval_ms = offset_ms;
    } else {
        mp_obj_t *items;
        mp_obj_get_array_fixed_n(args[ARG_time].u_obj, 8, &items);
        rtc_alarm_target_from_tuple(items, &a);
        self->alarm_target = (time_t)timeutils_seconds_since_epoch(
            mp_obj_get_int(items[0]), mp_obj_get_int(items[1]),
            mp_obj_get_int(items[2]), mp_obj_get_int(items[4]),
            mp_obj_get_int(items[5]), mp_obj_get_int(items[6]));
        // repeat is only defined for the millisecond-offset form; silently
        // ignored for datetimetuple per docs/library/machine.RTC.rst.
        self->alarm_repeat = false;
        self->alarm_interval_ms = 0;
    }

    rtc_set_alarm(&a, machine_rtc_alarm_irq_handler);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_rtc_alarm_obj, 2, machine_rtc_alarm);

static mp_obj_t machine_rtc_alarm_left(size_t n_args, const mp_obj_t *args) {
    if (n_args == 2 && mp_obj_get_int(args[1]) != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid alarm id"));
    }
    machine_rtc_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    return mp_obj_new_int_from_uint(rtc_alarm_ms_left(rtc_read(), self->alarm_target));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_rtc_alarm_left_obj, 1, 2, machine_rtc_alarm_left);

static mp_obj_t machine_rtc_alarm_cancel(size_t n_args, const mp_obj_t *args) {
    if (n_args == 2 && mp_obj_get_int(args[1]) != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid alarm id"));
    }
    machine_rtc_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    rtc_disable_alarm();
    self->alarm_target = 0;
    self->alarm_repeat = false;
    self->alarm_interval_ms = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_rtc_alarm_cancel_obj, 1, 2, machine_rtc_alarm_cancel);

// ---------------------------------------------------------------------------
// irq — register a Python callback for the RTC alarm (mirrors machine_uart.c's
// mp_irq framework usage; see shared/runtime/mpirq.h).
// ---------------------------------------------------------------------------
static mp_uint_t machine_rtc_irq_trigger(mp_obj_t self_in, mp_uint_t new_trigger) {
    (void)self_in;
    return new_trigger; // only RTC.ALARM0 (0) is valid; nothing to store per-trigger
}

static mp_uint_t machine_rtc_irq_info(mp_obj_t self_in, mp_uint_t info_type) {
    (void)self_in;
    (void)info_type;
    return 0; // no flags/trigger introspection exposed for RTC
}

static const mp_irq_methods_t machine_rtc_irq_methods = {
    .trigger = machine_rtc_irq_trigger,
    .info = machine_rtc_irq_info,
};

static mp_obj_t machine_rtc_irq(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_trigger, ARG_handler, ARG_wake };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_trigger, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_handler, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_wake,    MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = MP_MACHINE_WAKE_IDLE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_trigger].u_int != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("trigger must be RTC.ALARM0"));
    }
    // wake is accepted for API compatibility only; none of IDLE/SLEEP/
    // DEEPSLEEP currently function as a real wake source. Confirmed on
    // hardware on both boards: an armed RTC alarm does NOT wake the chip
    // out of a parameterless machine.lightsleep() (vTaskDelay(portMAX_DELAY)
    // requires an explicit task-resume call, not merely "any interrupt";
    // this RTC irq is dispatched via mp_sched_schedule(), which never runs
    // while the main task is blocked there) -- see ports/doc/quickref.rst
    // "RTC alarm and interrupts" for the user-facing writeup and workaround
    // (pass an explicit duration to lightsleep(ms) instead). deepsleep
    // remains unwakeable by this path for the unrelated reason that its
    // wake sources are limited to the AON timer / AON GPIO pins.

    machine_rtc_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_irq_obj_t *irq = MP_STATE_PORT(machine_rtc_irq_object);
    if (irq == NULL) {
        irq = mp_irq_new(&machine_rtc_irq_methods, MP_OBJ_FROM_PTR(self));
        MP_STATE_PORT(machine_rtc_irq_object) = irq;
    }
    irq->handler = args[ARG_handler].u_obj;
    irq->ishard = false;
    return MP_OBJ_FROM_PTR(irq);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(machine_rtc_irq_obj, 1, machine_rtc_irq);

// KNOWN HARDWARE LIMITATION (confirmed on PKE8721DAF/RTL8721Dx and
// EV8711FLM/RTL8711F): the vendor SDK's own ISR trampoline --
// rtc_alarm_intr_handler() in ameba-rtos component/soc/<soc>/hal/src/rtc_api.c
// -- unconditionally calls rtc_disable_alarm() right after invoking this
// handler:
//     RTC_AlarmClear();
//     if (rtc_alarm_handler != NULL) { hdl = rtc_alarm_handler; hdl(); }
//     rtc_disable_alarm();   // <-- tears down whatever hdl() just armed
// rtc_disable_alarm() unregisters the RTC IRQ and NULLs rtc_alarm_handler,
// so calling rtc_alarm_arm() synchronously from within this vendor-invoked
// callback is undone before the ISR returns: the alarm fires exactly once
// and never re-arms in practice, even though alarm_target/alarm_repeat
// bookkeeping is otherwise correct. Verified live on both boards via
// machine.RTC().alarm(0, 2000, repeat=True): alarm_left() dropped to 0 after
// the first fire and stayed 0 indefinitely instead of cycling back up every
// ~2s.
//
// FIX (this commit): the re-arm is deferred via mp_sched_schedule_node()
// (py/runtime.h) to run on the scheduler pump, which always executes after
// this ISR -- and thus after rtc_alarm_intr_handler()'s rtc_disable_alarm()
// call -- has returned. This handler itself only updates alarm_target and
// queues the scheduled callback below; the actual rtc_alarm_arm() call
// happens in machine_rtc_alarm_rearm_scheduled(), out of ISR context, once
// the vendor trampoline is done tearing down the previous arm. Re-verified
// live on both boards via the same alarm(0, 2000, repeat=True) test:
// alarm_left() now cycles back up towards ~2000 every ~2s instead of
// latching at 0 -- see .superpowers/sdd/task-3-report.md for the fix's
// verification log.
static void machine_rtc_alarm_rearm_scheduled(mp_sched_node_t *node) {
    (void)node;
    // Re-check alarm_repeat: alarm_cancel() (or a fresh alarm() call) may
    // have run between this node being queued from the ISR and it being
    // pumped here, and clears alarm_repeat without cancelling the already
    // -queued node. Without this check we'd re-arm hardware against a
    // stale/zeroed alarm_target after the caller asked to cancel.
    machine_rtc_obj_t *self = &machine_rtc_obj;
    if (self->alarm_repeat) {
        rtc_alarm_arm(self, machine_rtc_alarm_irq_handler);
    }
}

static void machine_rtc_alarm_irq_handler(void) {
    machine_rtc_obj_t *self = &machine_rtc_obj;

    if (self->alarm_repeat) {
        // Hardware alarm_t has no independent countdown register -- re-arm
        // for "fired-at + original interval" so drift doesn't accumulate
        // relative to wall-clock (matches mimxrt's machine_rtc_alarm_helper
        // re-programming approach). The actual rtc_alarm_arm() call is
        // deferred to machine_rtc_alarm_rearm_scheduled() (see comment
        // above) since calling it here, synchronously, would be undone by
        // the vendor ISR trampoline's unconditional rtc_disable_alarm().
        self->alarm_target += (time_t)(self->alarm_interval_ms / 1000);
        mp_sched_schedule_node(&machine_rtc_alarm_sched_node, machine_rtc_alarm_rearm_scheduled);
    } else {
        self->alarm_target = 0;
    }

    mp_irq_obj_t *irq = MP_STATE_PORT(machine_rtc_irq_object);
    if (irq != NULL) {
        mp_irq_handler(irq);
    }
}

static const mp_rom_map_elem_t machine_rtc_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_datetime),     MP_ROM_PTR(&machine_rtc_datetime_obj) },
    { MP_ROM_QSTR(MP_QSTR_alarm),        MP_ROM_PTR(&machine_rtc_alarm_obj) },
    { MP_ROM_QSTR(MP_QSTR_alarm_left),   MP_ROM_PTR(&machine_rtc_alarm_left_obj) },
    { MP_ROM_QSTR(MP_QSTR_alarm_cancel), MP_ROM_PTR(&machine_rtc_alarm_cancel_obj) },
    { MP_ROM_QSTR(MP_QSTR_irq),          MP_ROM_PTR(&machine_rtc_irq_obj) },
    { MP_ROM_QSTR(MP_QSTR_ALARM0),       MP_ROM_INT(0) },
};
static MP_DEFINE_CONST_DICT(machine_rtc_locals_dict, machine_rtc_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_rtc_type,
    MP_QSTR_RTC,
    MP_TYPE_FLAG_NONE,
    make_new, machine_rtc_make_new,
    locals_dict, &machine_rtc_locals_dict
    );

// Called from mp_main.c's soft_reset_exit, before gc_sweep_all() -- mirrors
// every other IRQ-capable peripheral's *_deinit_all() (UART/I2C/I2C-target/
// SPI/PWM/I2S). machine_rtc_obj and machine_rtc_alarm_sched_node are static
// BSS and survive soft reset with the vendor RTC ISR still armed if an
// alarm was live; left alone, a subsequent fire would dispatch through a
// swept mp_irq_obj (via the also-surviving root pointer) or find
// machine_rtc_alarm_sched_node.callback still non-NULL from an
// already-queued-but-not-yet-pumped rearm, which makes every future
// mp_sched_schedule_node() call silently return false (see
// micropython/py/scheduler.c) and permanently kills repeat re-arm.
void machine_rtc_deinit_all(void) {
    rtc_disable_alarm();
    machine_rtc_obj.alarm_target = 0;
    machine_rtc_obj.alarm_repeat = false;
    machine_rtc_obj.alarm_interval_ms = 0;
    machine_rtc_alarm_sched_node.callback = NULL;
    MP_STATE_PORT(machine_rtc_irq_object) = NULL;
}

MP_REGISTER_ROOT_POINTER(struct _mp_irq_obj_t *machine_rtc_irq_object);

// ---- mbedtls millisecond clock (see mbedtls_user_config.h) ----

// mbedtls's own platform_util.c only provides a built-in mbedtls_ms_time()
// for Linux/Windows hosts (clock_gettime()/GetSystemTimeAsFileTime()); on a
// bare-metal target with MBEDTLS_HAVE_TIME defined it hits
// `#error "No mbedtls_ms_time available"` unless MBEDTLS_PLATFORM_MS_TIME_ALT
// is defined and something supplies this function -- mirrors rp2's
// mbedtls_port.c. Reuses the same libc wall clock the datetime() setter
// above feeds via _settimeofday(), so this and the TLS certificate-date
// checks agree on what time it is. mbedtls_ms_time_t is int64_t by default
// (mbedtls/platform_time.h) and this port doesn't override that macro.
// Calls _gettimeofday() (the syscall), not gettimeofday() -- the public
// symbol is shadowed by iperf3's own same-named, unrelated, purely-uptime
// helper (see modtime.c).
int64_t mbedtls_ms_time(void) {
    struct timeval tv;
    _gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
