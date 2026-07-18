/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * Development of the code in this file was sponsored by Microbric Pty Ltd
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2023 Damien P. George
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

#include <sys/time.h>

#include "py/obj.h"
#include "shared/timeutils/timeutils.h"

// The public gettimeofday() symbol is shadowed by
// ameba-rtos/component/network/iperf3/timer.c's own same-named function
// (an unrelated, purely-uptime-based helper for iperf3's internal bandwidth
// timers, unconditionally linked in) -- calling gettimeofday() here would
// silently read iperf3's clock instead of the wall clock set via
// machine.RTC()/_settimeofday() (see machine_rtc.c). _gettimeofday() (the
// underlying syscall) doesn't collide, but sys/time.h only declares it
// under `#ifdef _COMPILING_NEWLIB`, so it needs a declaration here too.
int _gettimeofday(struct timeval *ptimeval, void *ptimezone);

static void mp_time_localtime_get(timeutils_struct_time_t *tm) {
    struct timeval tv;
    _gettimeofday(&tv, NULL);
    timeutils_seconds_since_epoch_to_struct_time(tv.tv_sec, tm);
}

// Return the number of seconds since the Epoch.
static mp_obj_t mp_time_time_get(void) {
    struct timeval tv;
    _gettimeofday(&tv, NULL);
    return timeutils_obj_from_timestamp(tv.tv_sec);
}
