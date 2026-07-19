/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Chester Tseng
 * Copyright (c) 2021 Huang Yue
 * Copyright (c) 2022 Simon Xi 
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

#include "ameba_flash.h"
#include "flash_api.h"
#include "stdio.h"
#include "extmod/vfs.h"
#include "py/mperrno.h"

#define SECTOR_SIZE_FLASH   4096    // 4KB per sector

// VFS1 region from menuconfig (CONFIG_FLASH_VFS1_* visible via flash_api.h include chain)
// flash_stream_* APIs expect relative offset from flash start, not absolute address
#define FS_START_OFFSET  (CONFIG_FLASH_VFS1_OFFSET - SPI_FLASH_BASE)
#define FS_SIZE          CONFIG_FLASH_VFS1_SIZE


/*****************************************************************************
 *                              External variables
 * ***************************************************************************/

/*****************************************************************************
 *                              Internal functions
 * ***************************************************************************/
const mp_obj_type_t flash_type;

static flash_obj_t amb_flash_obj = {
        .base = { &flash_type },
        .block_size = SECTOR_SIZE_FLASH,
        .start = FS_START_OFFSET,
        .len = FS_SIZE,
};


static void amb_flash_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    flash_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "Flash(start=0x%08x, len=%u)", self->start, self->len);
}

/* Read block(s) --------------------------------------------*/
mp_obj_t amb_flash_readblocks(size_t n_args, const mp_obj_t *args) {
    flash_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    int64_t offset = (int64_t)mp_obj_get_int(args[1]) * self->block_size;
    mp_buffer_info_t bufinfo;
    int res = 0;

    mp_get_buffer_raise(args[2], &bufinfo, MP_BUFFER_WRITE);

    if (n_args == 4) {
        offset += mp_obj_get_int(args[3]);
        //mp_raise_ValueError(MP_ERROR_TEXT("offset addressing not supported"));
    }

    // block_num (and the optional byte offset) come straight from the
    // Python caller -- flash_stream_read() has no bounds checking of its
    // own and will happily read from anywhere in the SoC's 32-bit address
    // space, so an out-of-range value here previously turned into a hard
    // Bus Fault / heap corruption instead of a clean OSError.
    if (offset < 0 || (uint64_t)offset + bufinfo.len > self->len) {
        mp_raise_OSError(MP_EIO);
    }

    res = flash_stream_read(NULL, self->start + (uint32_t)offset, bufinfo.len, (uint8_t *) bufinfo.buf);
    if (res != 1) {  /* SDK returns 1 on success, not 0 */
        mp_raise_OSError(MP_EIO);
    }
    return MP_OBJ_NEW_SMALL_INT(0);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(amb_flash_readblocks_obj, 3, 4, amb_flash_readblocks);

/* Write block(s) --------------------------------------------*/
mp_obj_t amb_flash_writeblocks(size_t n_args, const mp_obj_t *args) {
    flash_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    int64_t block_num = mp_obj_get_int(args[1]);
    int64_t offset = block_num * self->block_size;
    mp_buffer_info_t bufinfo;
    int res = 0;

    mp_get_buffer_raise(args[2], &bufinfo, MP_BUFFER_READ);

    if (n_args == 4) {
        offset += mp_obj_get_int(args[3]);
    } else if (bufinfo.len % self->block_size != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("write length must be a multiple of block size"));
    }

    // block_num (and the optional byte offset) come straight from the
    // Python caller -- flash_erase_sector()/flash_stream_write() have no
    // bounds checking of their own, so an out-of-range value here
    // previously turned into a real erase/write at an unintended flash
    // address (or a hard Bus Fault / heap corruption) instead of a clean
    // OSError.
    if (offset < 0 || (uint64_t)offset + bufinfo.len > self->len) {
        mp_raise_OSError(MP_EIO);
    }

    if (n_args != 4) {
        int count = bufinfo.len / self->block_size;
        for (int i = 0; i < count; i++) {
            uint32_t erase_addr = self->start + (uint32_t)(block_num + i) * SECTOR_SIZE_FLASH;
            flash_erase_sector(NULL, erase_addr);
        }
    }

    res = flash_stream_write(NULL, self->start + (uint32_t)offset, bufinfo.len, (uint8_t *) bufinfo.buf);
    if (res != 1) {  /* SDK returns 1 on success, not 0 */
        mp_raise_OSError(MP_EIO);
    }
    return MP_OBJ_NEW_SMALL_INT(0);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(amb_flash_writeblocks_obj, 3, 4, amb_flash_writeblocks);


/* IOCTL blocks(s) --------------------------------------------*/
static mp_obj_t amb_flash_ioctl (mp_obj_t self_in, mp_obj_t cmd_in, mp_obj_t arg_in){
    flash_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t cmd = mp_obj_get_int(cmd_in);
    
    switch (cmd) {
        case MP_BLOCKDEV_IOCTL_INIT: {
            return MP_OBJ_NEW_SMALL_INT(0);
        }
        case MP_BLOCKDEV_IOCTL_DEINIT: {
            return MP_OBJ_NEW_SMALL_INT(0);
        }
        case MP_BLOCKDEV_IOCTL_SYNC: {
            return MP_OBJ_NEW_SMALL_INT(0);
        }
        //Get sector count
        case MP_BLOCKDEV_IOCTL_BLOCK_COUNT: {
            return MP_OBJ_NEW_SMALL_INT((mp_int_t)(self->len / self->block_size));
        }
        //Get sector size
        case MP_BLOCKDEV_IOCTL_BLOCK_SIZE: {
            return MP_OBJ_NEW_SMALL_INT(self->block_size);
        }
        case MP_BLOCKDEV_IOCTL_BLOCK_ERASE: {
            int64_t block_num = mp_obj_get_int(arg_in);
            // Same out-of-range guard as amb_flash_writeblocks() -- see its
            // comment for why this matters (real erase at an unintended
            // flash address / hard crash instead of a clean OSError).
            if (block_num < 0 || (uint64_t)(block_num + 1) * SECTOR_SIZE_FLASH > self->len) {
                mp_raise_OSError(MP_EIO);
            }
            uint32_t erase_addr = self->start + (uint32_t)block_num * SECTOR_SIZE_FLASH;
            /* erase_addr is relative offset from SPI_FLASH_BASE, same convention as flash_stream_read/write */
            flash_erase_sector(NULL, erase_addr);
            return MP_OBJ_NEW_SMALL_INT(0);
        }
        default: {
            return mp_const_none;
        }
    }
}

static MP_DEFINE_CONST_FUN_OBJ_3(amb_flash_ioctl_obj, amb_flash_ioctl);


static void amb_flash_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] == MP_OBJ_NULL) {
        flash_obj_t *self = MP_OBJ_TO_PTR(self_in);
        if (attr == MP_QSTR_start) {
            dest[0] = mp_obj_new_int_from_uint(self->start);
        } else if (attr == MP_QSTR_len) {
            dest[0] = mp_obj_new_int_from_uint(self->len);
        } else if (attr == MP_QSTR_block_size) {
            dest[0] = mp_obj_new_int_from_uint(self->block_size);
        } else {
            /* Not handled here — signal MicroPython to fall through to locals_dict. */
            dest[1] = MP_OBJ_SENTINEL;
        }
    }
}

static mp_obj_t amb_flash_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    // Check args.
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    // Return singleton object.
    return MP_OBJ_FROM_PTR(&amb_flash_obj);
}


static const mp_rom_map_elem_t amb_flash_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_readblocks), MP_ROM_PTR(&amb_flash_readblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeblocks), MP_ROM_PTR(&amb_flash_writeblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_ioctl), MP_ROM_PTR(&amb_flash_ioctl_obj) },
};
static MP_DEFINE_CONST_DICT(amb_flash_locals_dict, amb_flash_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    flash_type,
    MP_QSTR_Flash,
    MP_TYPE_FLAG_NONE,
    make_new, amb_flash_make_new,
    print, amb_flash_print,
    attr, amb_flash_attr,
    locals_dict, &amb_flash_locals_dict
);
