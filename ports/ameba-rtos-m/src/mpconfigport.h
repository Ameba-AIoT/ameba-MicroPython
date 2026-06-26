/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Realtek Corporation
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
#include <stdlib.h>
#include "ameba_soc.h"
// Python internal features.
#ifndef MICROPY_CONFIG_ROM_LEVEL
#define MICROPY_CONFIG_ROM_LEVEL            (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)
#endif

// task size
#ifndef MICROPY_TASK_STACK_SIZE
#define MICROPY_TASK_STACK_SIZE             (32 * 1024)
#endif

#define MICROPY_GC_INITIAL_HEAP_SIZE        (64 * 1024)

// modify default mp define
//value
#define MICROPY_ALLOC_PATH_MAX              (128)
// bool
#define MICROPY_PERSISTENT_CODE_LOAD        (1)
#define MICROPY_EMIT_THUMB                      (1)
#define MICROPY_EMIT_INLINE_THUMB               (1)

// #define MICROPY_EMIT_THUMB_ARMV7M               (0)
// #define MICROPY_EMIT_INLINE_THUMB_FLOAT         (0)
#define MICROPY_MAKE_POINTER_CALLABLE(p) ((void *)((uint32_t)(p) | 1))

// #define MICROPY_EMIT_NATIVE_DEBUG   (1)
// #define MICROPY_EMIT_NATIVE_DEBUG_PRINTER (0)

#define MICROPY_OPT_COMPUTED_GOTO           (1)
#define MICROPY_ENABLE_GC                       (1)
#define MICROPY_READER_VFS                      (1)
#define MICROPY_STACK_CHECK_MARGIN              (1024)
#define MICROPY_WARNINGS                        (1)
#define MICROPY_VFS                             (1)
#define MICROPY_ERROR_REPORTING                 (MICROPY_ERROR_REPORTING_DETAILED)
#define MICROPY_FLOAT_IMPL                      (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF  (1)
#define MICROPY_STREAMS_POSIX_API               (1)
#define MICROPY_SCHEDULER_DEPTH                 (8)
#define MICROPY_SCHEDULER_STATIC_NODES          (1)

// fatfs configuration
#define MICROPY_FATFS_ENABLE_LFN            (1)
#define MICROPY_FATFS_RPATH                 (2)
#define MICROPY_FATFS_MAX_SS                (4096)
#define MICROPY_FATFS_LFN_CODE_PAGE         437 /* 1=SFN/ANSI 437=LFN/U.S.(OEM) */
// todo
#define MICROPY_FATFS_NORTC                 (1)

// control over Python builtins
#define MICROPY_PY_STR_BYTES_CMP_WARN       (1)
#define MICROPY_PY_ALL_INPLACE_SPECIAL_METHODS (1)
#define MICROPY_PY_BUILTINS_HELP_TEXT       ameba_help_text
#define MICROPY_PY_IO_BUFFEREDWRITER        (1)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (1)
#define MICROPY_PY_TIME_TIME_TIME_NS        (1)
#define MICROPY_PY_TIME_INCLUDEFILE         "src/modtime.c"
#define MICROPY_PY_THREAD                   (1)
#define MICROPY_PY_THREAD_GIL               (1)
#define MICROPY_PY_THREAD_GIL_VM_DIVISOR    (32)
#define MICROPY_GC_SPLIT_HEAP               (1)
#define MICROPY_GC_SPLIT_HEAP_AUTO          (1)

// EXTRA_FEATURES extra
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC    (_rand())
// modos.c is removed: urandom is handled by extmod via mp_hal_get_random() below
// #define MICROPY_PY_OS_DUPTERM               (1)
// #define MICROPY_PY_OS_DUPTERM_NOTIFY        (1)
#define MICROPY_PY_OS_SYNC                  (1)
#define MICROPY_PY_OS_UNAME                 (1)
#define MICROPY_PY_OS_URANDOM               (1)
// board specifics
#define MICROPY_PY_SYS_PLATFORM             "ameba"

// REPL/LOGUART 波特率。ROM 启动时把 LOGUART 拉到 1500000；定义此宏后
// rtk_loguart_init() 会切换到指定波特率，注释掉则沿用 ROM 默认（1500000）。
// 设为 115200 可让官方 run-multitests.py（经 pyboard 硬编码 115200）原生连板。
#define MICROPY_HW_LOGUART_BAUDRATE         (115200)

// machine
#define MICROPY_PY_MACHINE                  (1)
#define MICROPY_PY_MACHINE_INCLUDEFILE      "src/modmachine.c"
#define MICROPY_PY_MACHINE_RESET            (1)
#define MICROPY_PY_MACHINE_BARE_METAL_FUNCS (1)
#define MICROPY_PY_MACHINE_DISABLE_IRQ_ENABLE_IRQ (1)
#define MICROPY_PY_MACHINE_PIN_MAKE_NEW     mp_pin_make_new

#define MICROPY_PY_MACHINE_UART              (1)
#define MICROPY_PY_MACHINE_UART_INCLUDEFILE  "src/machine_uart.c"
#define MICROPY_PY_MACHINE_UART_IRQ          (0)
#define MICROPY_PY_MACHINE_I2C               (1)
#define MICROPY_PY_MACHINE_ADC               (1)
#define MICROPY_PY_MACHINE_ADC_INCLUDEFILE   "src/machine_adc.c"
#define MICROPY_PY_MACHINE_ADC_DEINIT        (1)
#define MICROPY_PY_MACHINE_PWM               (1)
#define MICROPY_PY_MACHINE_PWM_INCLUDEFILE   "src/machine_pwm.c"


#define MICROPY_PY_NETWORK (1)
#define MICROPY_PY_NETWORK_WLAN                        (1)
#define MICROPY_PY_NETWORK_INCLUDEFILE                 "src/modnetwork.h"
#define MICROPY_PY_NETWORK_MODULE_GLOBALS_INCLUDEFILE  "src/modnetwork_globals.h"
#ifndef MICROPY_PY_NETWORK_HOSTNAME_DEFAULT
#define MICROPY_PY_NETWORK_HOSTNAME_DEFAULT "mpy-ameba"
#endif
#define MICROPY_PY_SSL                      (1)
#define MICROPY_SSL_MBEDTLS                 (1)
#define MICROPY_PY_WEBSOCKET                (1)
#define MICROPY_PY_WEBREPL                  (1)
// #define MICROPY_PY_ONEWIRE                  (1)
#define MICROPY_PY_SOCKET_EVENTS            (0)
// #define MICROPY_PY_BLUETOOTH_RANDOM_ADDR    (1)


#define MICROPY_PY_MATH_GAMMA_FIX_NEGINF (1)

// #ifndef MICROPY_HW_ENABLE_MDNS_QUERIES
// #define MICROPY_HW_ENABLE_MDNS_QUERIES      (1)
// #endif

// #ifndef MICROPY_HW_ENABLE_MDNS_RESPONDER
// #define MICROPY_HW_ENABLE_MDNS_RESPONDER    (1)
// #endif

typedef long mp_off_t;

// We need to provide a declaration/definition of alloca().
#include <alloca.h>

// Define the port's name and hardware.
// These are overridden per-board by boards/<BOARD>/mpconfigboard.h.
#ifndef MICROPY_HW_BOARD_NAME
#define MICROPY_HW_BOARD_NAME "ameba-board"
#endif
#ifndef MICROPY_HW_MCU_NAME
#define MICROPY_HW_MCU_NAME   "ameba-cpu"
#endif

#define MP_STATE_PORT MP_STATE_VM

#define MP_SSIZE_MAX (0x7fffffff)

#if MICROPY_PY_SOCKET_EVENTS
#define MICROPY_PY_SOCKET_EVENTS_HANDLER extern void socket_events_handler(void); socket_events_handler();
#else
#define MICROPY_PY_SOCKET_EVENTS_HANDLER
#endif



#if MICROPY_PY_THREAD
#define MICROPY_EVENT_POLL_HOOK \
    do { \
        mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS); \
        MP_THREAD_GIL_EXIT(); \
        ulTaskNotifyTake(pdFALSE, 1); \
        MP_THREAD_GIL_ENTER(); \
    } while (0);
#else
#define MICROPY_PY_WAIT_FOR_INTERRUPT __ASM volatile ("wfi":::"memory")
#define MICROPY_EVENT_POLL_HOOK \
    do { \
        mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS); \
        MICROPY_PY_WAIT_FOR_INTERRUPT; \
    } while (0);
#endif


#define MICROPY_LONGINT_IMPL                (MICROPY_LONGINT_IMPL_MPZ)

#define MP_HAL_CLEAN_DCACHE(addr, size) DCache_Clean((uint32_t)(uintptr_t)(addr), (uint32_t)(size))

#define MICROPY_WRAP_MP_EXECUTE_BYTECODE(f) SRAM_ONLY_TEXT_SECTION f
#define MICROPY_WRAP_MP_LOAD_GLOBAL(f) SRAM_ONLY_TEXT_SECTION f
#define MICROPY_WRAP_MP_LOAD_NAME(f) SRAM_ONLY_TEXT_SECTION f
#define MICROPY_WRAP_MP_MAP_LOOKUP(f) SRAM_ONLY_TEXT_SECTION f
#define MICROPY_WRAP_MP_OBJ_GET_TYPE(f) SRAM_ONLY_TEXT_SECTION f
#define MICROPY_WRAP_MP_SCHED_EXCEPTION(f) SRAM_ONLY_TEXT_SECTION f
#define MICROPY_WRAP_MP_SCHED_KEYBOARD_INTERRUPT(f) SRAM_ONLY_TEXT_SECTION f


#define MICROPY_DEBUG_PRINTERS (0)
#define MICROPY_DEBUG_PARSE_RULE_NAME (0)
