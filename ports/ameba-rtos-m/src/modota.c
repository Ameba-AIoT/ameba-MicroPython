// src/modota.c — ameba.OTA: streaming OTA upgrade via SDK OTA_USER path.

#include <string.h>
#include "ameba.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "stream_buffer.h"
#include "task.h"
#include "ota_api.h"
#include "ameba_ota.h"   // ota_get_cur_index, OTA_IMGID_APP
#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/mphal.h"

#define OTA_STREAM_BUF_SIZE     (8 * 1024)
#define OTA_TASK_STACK_WORDS    (4096 / sizeof(StackType_t))
#define OTA_TASK_PRIO           2

#define OTA_RESULT_PENDING  (-1)
#define OTA_RESULT_OK       (0)
#define OTA_RESULT_ERR      (1)

typedef struct _ameba_ota_obj_t {
    mp_obj_base_t base;
    ota_context_t ctx;
    StreamBufferHandle_t stream;
    SemaphoreHandle_t sem_done;
    TaskHandle_t task;
    volatile int result;
    volatile bool eof;
    bool finished;
} ameba_ota_obj_t;

static void ameba_ota_task_entry(void *arg);

// Called by ota_start() loop inside the OTA task to pull the next firmware chunk.
// Polls the StreamBuffer every 50 ms; returns 0 when eof is set and buffer is empty.
static int ota_user_read_cb(u8 *buf, int len) {
    ameba_ota_obj_t *self = MP_STATE_PORT(ota_active);
    if (!self) {
        return -1;
    }
    for (;;) {
        size_t n = xStreamBufferReceive(self->stream, buf, (size_t)len,
                                        pdMS_TO_TICKS(50));
        if (n > 0) {
            return (int)n;
        }
        if (self->eof) {
            // Drain any bytes written just before eof was set.
            n = xStreamBufferReceive(self->stream, buf, (size_t)len, 0);
            return (int)n;  // 0 = EOF signal to ota_start
        }
    }
}

// FreeRTOS task body: runs the blocking ota_start() loop, then signals completion.
static void ameba_ota_task_entry(void *arg) {
    ameba_ota_obj_t *self = (ameba_ota_obj_t *)arg;
    ota_register_user_read_func(&self->ctx, ota_user_read_cb);
    int ret = ota_start(&self->ctx);
    self->result = (ret == OTA_OK) ? OTA_RESULT_OK : OTA_RESULT_ERR;
    xSemaphoreGive(self->sem_done);
    vTaskDelete(NULL);
}

static mp_obj_t ameba_ota_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);

    if (MP_STATE_PORT(ota_active) != NULL &&
        MP_STATE_PORT(ota_active)->result == OTA_RESULT_PENDING) {
        mp_raise_OSError(MP_EBUSY);
    }

    ameba_ota_obj_t *self = mp_obj_malloc_with_finaliser(ameba_ota_obj_t, type);
    memset(&self->ctx, 0, sizeof(ota_context_t));
    self->stream   = NULL;
    self->sem_done = NULL;
    self->task     = NULL;
    self->result   = OTA_RESULT_PENDING;
    self->eof      = false;
    self->finished = false;

    if (ota_init(&self->ctx, NULL, 0, NULL, OTA_USER) != OTA_OK) {
        mp_raise_OSError(MP_EIO);
    }

    self->stream = xStreamBufferCreate(OTA_STREAM_BUF_SIZE, 1);
    if (!self->stream) {
        ota_deinit(&self->ctx);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("OTA buffer alloc failed"));
    }

    self->sem_done = xSemaphoreCreateBinary();
    if (!self->sem_done) {
        vStreamBufferDelete(self->stream);
        ota_deinit(&self->ctx);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("OTA semaphore alloc failed"));
    }

    MP_STATE_PORT(ota_active) = self;

    if (xTaskCreate(ameba_ota_task_entry, "ota_task",
            OTA_TASK_STACK_WORDS, self, OTA_TASK_PRIO, &self->task) != pdPASS) {
        vSemaphoreDelete(self->sem_done);
        vStreamBufferDelete(self->stream);
        ota_deinit(&self->ctx);
        MP_STATE_PORT(ota_active) = NULL;
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("OTA task create failed"));
    }

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t ameba_ota_del(mp_obj_t self_in) {
    ameba_ota_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->result == OTA_RESULT_PENDING) {
        self->eof = true;
        if (self->stream) {
            // Wake the OTA task's 50ms poll so it sees eof=true and exits.
            // xStreamBufferReset is intentionally omitted: it returns pdFAIL
            // if the task is currently blocked in xStreamBufferReceive, which
            // would silently leave stale data and corrupt the abort path.
            uint8_t dummy = 0;
            MP_THREAD_GIL_EXIT();
            xStreamBufferSend(self->stream, &dummy, 1, pdMS_TO_TICKS(100));
            MP_THREAD_GIL_ENTER();
        }
        if (self->sem_done) {
            MP_THREAD_GIL_EXIT();
            xSemaphoreTake(self->sem_done, pdMS_TO_TICKS(10000));
            MP_THREAD_GIL_ENTER();
        }
    }
    if (self->sem_done) { vSemaphoreDelete(self->sem_done); self->sem_done = NULL; }
    if (self->stream)   { vStreamBufferDelete(self->stream);  self->stream = NULL; }
    ota_deinit(&self->ctx);
    if (MP_STATE_PORT(ota_active) == self) { MP_STATE_PORT(ota_active) = NULL; }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ameba_ota_del_obj, ameba_ota_del);

// write(chunk) — push bytes into the OTA stream buffer.
// Blocks (in 100 ms slices, GIL released) until all bytes are accepted.
// Raises OSError(EIO) if the OTA task has already failed.
static mp_obj_t ameba_ota_write(mp_obj_t self_in, mp_obj_t data_in) {
    ameba_ota_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->finished) {
        mp_raise_OSError(MP_EALREADY);
    }
    if (self->result != OTA_RESULT_PENDING) {
        mp_raise_OSError(MP_EIO);
    }
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);
    if (bufinfo.len == 0) {
        return mp_const_none;
    }
    const uint8_t *ptr = (const uint8_t *)bufinfo.buf;
    size_t remaining = bufinfo.len;
    while (remaining > 0) {
        if (self->result != OTA_RESULT_PENDING) {
            mp_raise_OSError(MP_EIO);
        }
        MP_THREAD_GIL_EXIT();
        size_t sent = xStreamBufferSend(self->stream, ptr, remaining,
                                        pdMS_TO_TICKS(100));
        MP_THREAD_GIL_ENTER();
        ptr += sent;
        remaining -= sent;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ameba_ota_write_obj, ameba_ota_write);

// finish() — signal EOF, wait for OTA task to complete, check result.
// On success: firmware written and manifest activated; caller should machine.reset().
// On failure: raises OSError(EIO); backup slot is NOT activated.
static mp_obj_t ameba_ota_finish(mp_obj_t self_in) {
    ameba_ota_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->finished) {
        mp_raise_OSError(MP_EALREADY);
    }
    self->finished = true;
    self->eof = true;

    // Wait for OTA task to drain remaining buffer data, complete checksum,
    // write manifest, and give sem_done.
    MP_THREAD_GIL_EXIT();
    xSemaphoreTake(self->sem_done, portMAX_DELAY);
    MP_THREAD_GIL_ENTER();

    // Task done — clear active pointer so new sessions can be created immediately.
    if (MP_STATE_PORT(ota_active) == self) {
        MP_STATE_PORT(ota_active) = NULL;
    }

    if (self->result != OTA_RESULT_OK) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ameba_ota_finish_obj, ameba_ota_finish);

// cur_slot() — return which OTA slot the running firmware booted from.
// Returns 1 for OTA slot 1, 2 for OTA slot 2.
static mp_obj_t ameba_ota_cur_slot(mp_obj_t cls_in) {
    u8 idx = ota_get_cur_index(OTA_IMGID_APP);
    return MP_OBJ_NEW_SMALL_INT(idx + 1);  // OTA_INDEX_1=0 → 1, OTA_INDEX_2=1 → 2
}
static MP_DEFINE_CONST_FUN_OBJ_1(ameba_ota_cur_slot_fun_obj, ameba_ota_cur_slot);
static MP_DEFINE_CONST_CLASSMETHOD_OBJ(ameba_ota_cur_slot_obj,
    MP_ROM_PTR(&ameba_ota_cur_slot_fun_obj));

static const mp_rom_map_elem_t ameba_ota_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__),  MP_ROM_PTR(&ameba_ota_del_obj) },
    { MP_ROM_QSTR(MP_QSTR_write),    MP_ROM_PTR(&ameba_ota_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_finish),   MP_ROM_PTR(&ameba_ota_finish_obj) },
    { MP_ROM_QSTR(MP_QSTR_cur_slot), MP_ROM_PTR(&ameba_ota_cur_slot_obj) },
};
static MP_DEFINE_CONST_DICT(ameba_ota_locals_dict, ameba_ota_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    ameba_ota_type,
    MP_QSTR_OTA,
    MP_TYPE_FLAG_NONE,
    make_new, ameba_ota_make_new,
    locals_dict, &ameba_ota_locals_dict
);

// Keep the active OTA object reachable by the GC so it is not collected
// while the background task is still running.
MP_REGISTER_ROOT_POINTER(struct _ameba_ota_obj_t *ota_active);
