// SPDX-License-Identifier: MIT
// machine.SDCard port glue for ameba-rtos (AmebaGreen2 / RTL8711F only --
// AmebaDplus has no SD host controller hardware at all, see
// mpconfigport.h).
//
// EV8711FLM's on-board microSD socket is wired to SDIO_PAD Group2 (CLK=
// PA18/CMD=PA19/D0=PA28/D1=PA20/D2=PA27/D3=PA21, CD=PB19) -- confirmed
// against UG1002_EV721FL0_User_Guide_EVB_v1.2.pdf Table 3-2, which lists
// exactly these 7 pins as "if used as GPIO, SDIO interface on the EVB
// cannot work". The vendor's compiled-in default (ameba_intfcfg.c) is
// Group4 (PA6-PA11), which is wrong for this board and overlaps this
// port's UART1/SPI0 default pins -- must override SDH_Pin_Grp before
// calling SD_Init(). CD pin default (_PB_19) is already correct.
//
// SD_Init()/SD_ReadBlocks()/SD_WriteBlocks() are fully synchronous,
// blocking calls -- unlike machine.CAN (init-order deadlock risk) or
// network.LAN (async eth_init() with no public completion signal), there
// is no cross-call ordering hazard here beyond the SDH_Pin_Grp-before-
// SD_Init() rule above.
#include "py/runtime.h"
#include "py/mperrno.h"
#include "extmod/vfs.h"
#include "ameba_soc.h"

#include "machine_sdcard.h"

// Whole-file guard: machine_sdcard.c is unconditionally listed in
// CMakeLists.txt's private_sources (compiled for both boards), but
// AmebaDplus's ameba_soc.h doesn't declare any of SD_Init()/SDH_Pin_Grp/
// sdioh_config/SD_RESULT at all (no ameba_sd.h/ameba_intfcfg.h in that
// SoC's include tree) -- referencing them unconditionally would fail to
// compile/link on PKE8721DAF. Mirrors the machine_bitstream.c precedent.
#if MICROPY_PY_MACHINE_SDCARD

/* ---------- global singleton -------------------------------------------- */

typedef struct _machine_sdcard_obj_t {
    mp_obj_base_t base;
} machine_sdcard_obj_t;

static machine_sdcard_obj_t machine_sdcard_obj = {
    .base = { &machine_sdcard_type },
};

/* Tracks whether SD_Init() has succeeded -- SD_Init() itself doesn't raise
 * on SD_NODISK (no card inserted), so info()/ioctl(BLOCK_COUNT/BLOCK_SIZE)
 * need their own way to tell "never initialized" apart from "initialized
 * with a real capacity". */
static bool machine_sdcard_initialized = false;

/* Shared by make_new() and init() -- safe to call repeatedly even after a
 * previous failed attempt (e.g. retrying after inserting a card): SD_Init()
 * only re-runs its one-time clock/pinmux setup when hsd0.State is still
 * SD_STATE_RESET, then always re-runs SDIO_ResetAll/SDIOH_Init/the CD-pin
 * check/SD_PowerON/SD_CardInit, which is exactly the "try again" behavior
 * this needs. */
static bool machine_sdcard_try_init(void) {
    if (machine_sdcard_initialized) {
        return true;
    }
    SDH_Pin_Grp = 0x2;
    machine_sdcard_initialized = (SD_Init() == SD_OK);
    return machine_sdcard_initialized;
}

/* ---------- make_new ------------------------------------------------------ */

static mp_obj_t machine_sdcard_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    enum { ARG_slot };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_slot, MP_ARG_INT, {.u_int = 1} },
    };

    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, args, &kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    if (parsed[ARG_slot].u_int != 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("SDCard(slot) must be 1 -- this board has one SD host controller"));
    }

    /* SD_NODISK (no card inserted) is not an error here -- present()/init()
     * let the caller find out and retry once a card is inserted. */
    machine_sdcard_try_init();

    return MP_OBJ_FROM_PTR(&machine_sdcard_obj);
}

/* ---------- init()/deinit()/present() -------------------------------------- */

static mp_obj_t machine_sdcard_init(mp_obj_t self_in) {
    (void)self_in;
    machine_sdcard_try_init();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_sdcard_init_obj, machine_sdcard_init);

static mp_obj_t machine_sdcard_deinit(mp_obj_t self_in) {
    (void)self_in;
    SD_DeInit();
    machine_sdcard_initialized = false;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_sdcard_deinit_obj, machine_sdcard_deinit);

static mp_obj_t machine_sdcard_present(mp_obj_t self_in) {
    (void)self_in;
    /* SD_Init() itself treats a high CD pin reading as "no card" (see
     * ameba_sd.c) -- same polarity here. */
    return mp_obj_new_bool(GPIO_ReadDataBit(sdioh_config.sdioh_cd_pin) == 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_sdcard_present_obj, machine_sdcard_present);

/* ---------- info() ---------------------------------------------------------- */

static mp_obj_t machine_sdcard_info(mp_obj_t self_in) {
    (void)self_in;
    if (!machine_sdcard_initialized) {
        return mp_const_none;
    }
    u32 sector_count = 0;
    u32 sector_size = 0;
    if (SD_GetCapacity(&sector_count) != SD_OK || SD_GetSectorSize(&sector_size) != SD_OK) {
        return mp_const_none;
    }
    mp_obj_t tuple[2] = {
        mp_obj_new_int_from_ull((uint64_t)sector_count * (uint64_t)sector_size),
        mp_obj_new_int_from_uint(sector_size),
    };
    return mp_obj_new_tuple(2, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_1(machine_sdcard_info_obj, machine_sdcard_info);

/* ---------- readblocks()/writeblocks() -------------------------------------- */

static mp_obj_t machine_sdcard_readblocks(mp_obj_t self_in, mp_obj_t block_num_in, mp_obj_t buf_in) {
    (void)self_in;
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_WRITE);
    u32 count = bufinfo.len / SD_BLOCK_SIZE;
    if (SD_ReadBlocks(mp_obj_get_int(block_num_in), (u8 *)bufinfo.buf, count) == SD_OK) {
        return MP_OBJ_NEW_SMALL_INT(0);
    }
    return MP_OBJ_NEW_SMALL_INT(-MP_EIO);
}
static MP_DEFINE_CONST_FUN_OBJ_3(machine_sdcard_readblocks_obj, machine_sdcard_readblocks);

static mp_obj_t machine_sdcard_writeblocks(mp_obj_t self_in, mp_obj_t block_num_in, mp_obj_t buf_in) {
    (void)self_in;
    mp_buffer_info_t bufinfo;
    /* MP_BUFFER_READ, not _WRITE -- writeblocks() only reads from buf_in to
     * push it out to the card. mimxrt's machine_sdcard_writeblocks() passes
     * MP_BUFFER_WRITE here, which would wrongly reject read-only buffers
     * (e.g. a bytes literal). This port's own ameba_flash.c
     * amb_flash_writeblocks() already uses MP_BUFFER_READ for the same
     * operation -- follow that precedent instead. */
    mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_READ);
    u32 count = bufinfo.len / SD_BLOCK_SIZE;
    if (SD_WriteBlocks(mp_obj_get_int(block_num_in), (const u8 *)bufinfo.buf, count) == SD_OK) {
        return MP_OBJ_NEW_SMALL_INT(0);
    }
    return MP_OBJ_NEW_SMALL_INT(-MP_EIO);
}
static MP_DEFINE_CONST_FUN_OBJ_3(machine_sdcard_writeblocks_obj, machine_sdcard_writeblocks);

/* ---------- ioctl() ---------------------------------------------------------- */

static mp_obj_t machine_sdcard_ioctl(mp_obj_t self_in, mp_obj_t cmd_in, mp_obj_t arg_in) {
    (void)self_in;
    (void)arg_in;
    mp_int_t cmd = mp_obj_get_int(cmd_in);
    switch (cmd) {
        case MP_BLOCKDEV_IOCTL_INIT:
            return MP_OBJ_NEW_SMALL_INT(machine_sdcard_try_init() ? 0 : -MP_EIO);
        case MP_BLOCKDEV_IOCTL_DEINIT:
            SD_DeInit();
            machine_sdcard_initialized = false;
            return MP_OBJ_NEW_SMALL_INT(0);
        case MP_BLOCKDEV_IOCTL_SYNC:
            /* SD_ReadBlocks()/SD_WriteBlocks() are blocking calls that only
             * return once the transfer is complete (SD_WaitTransDone()) --
             * nothing left to flush. */
            return MP_OBJ_NEW_SMALL_INT(0);
        case MP_BLOCKDEV_IOCTL_BLOCK_COUNT: {
            if (!machine_sdcard_initialized) {
                return MP_OBJ_NEW_SMALL_INT(-MP_EIO);
            }
            u32 sector_count = 0;
            if (SD_GetCapacity(&sector_count) != SD_OK) {
                return MP_OBJ_NEW_SMALL_INT(-MP_EIO);
            }
            return mp_obj_new_int_from_uint(sector_count);
        }
        case MP_BLOCKDEV_IOCTL_BLOCK_SIZE: {
            if (!machine_sdcard_initialized) {
                return MP_OBJ_NEW_SMALL_INT(-MP_EIO);
            }
            u32 sector_size = 0;
            if (SD_GetSectorSize(&sector_size) != SD_OK) {
                return MP_OBJ_NEW_SMALL_INT(-MP_EIO);
            }
            return mp_obj_new_int_from_uint(sector_size);
        }
        default:
            return mp_const_none;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_3(machine_sdcard_ioctl_obj, machine_sdcard_ioctl);

/* ---------- locals_dict ---------------------------------------------------- */

static const mp_rom_map_elem_t machine_sdcard_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init),        MP_ROM_PTR(&machine_sdcard_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),      MP_ROM_PTR(&machine_sdcard_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_present),     MP_ROM_PTR(&machine_sdcard_present_obj) },
    { MP_ROM_QSTR(MP_QSTR_info),        MP_ROM_PTR(&machine_sdcard_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_readblocks),  MP_ROM_PTR(&machine_sdcard_readblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeblocks), MP_ROM_PTR(&machine_sdcard_writeblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_ioctl),       MP_ROM_PTR(&machine_sdcard_ioctl_obj) },
};
static MP_DEFINE_CONST_DICT(machine_sdcard_locals_dict, machine_sdcard_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_sdcard_type,
    MP_QSTR_SDCard,
    MP_TYPE_FLAG_NONE,
    make_new, machine_sdcard_make_new,
    locals_dict, &machine_sdcard_locals_dict
    );

#endif // MICROPY_PY_MACHINE_SDCARD
