// SPDX-License-Identifier: MIT
// get_fattime() for ameba-rtos: feeds oofatfs mtime/ctime stamps from the
// same hardware RTC that backs machine.RTC (see machine_rtc.c). Lazily
// initializes the RTC on first call so timestamps advance even if the user
// never touches machine.RTC -- matches ports/esp32 and ports/rp2, which
// source get_fattime() from an always-running clock rather than requiring
// the user to set one up first.

#include "lib/oofatfs/ff.h"
#include "shared/timeutils/timeutils.h"

#include "rtc_api.h"

DWORD get_fattime(void) {
    if (!rtc_isenabled()) {
        rtc_init();
    }
    timeutils_struct_time_t tm;
    timeutils_seconds_since_epoch_to_struct_time(rtc_read(), &tm);
    return ((DWORD)(tm.tm_year - 1980) << 25) | ((DWORD)tm.tm_mon << 21) | ((DWORD)tm.tm_mday << 16) |
           ((DWORD)tm.tm_hour << 11) | ((DWORD)tm.tm_min << 5) | ((DWORD)tm.tm_sec >> 1);
}
