/*
 * This file is part of the MicroPython project, http://micropython.org/
 * The MIT License (MIT) — Copyright (c) 2024 Realtek Corporation
 */
#ifndef MICROPY_INCLUDED_AMEBA_NETWORK_LAN_H
#define MICROPY_INCLUDED_AMEBA_NETWORK_LAN_H

#include "py/obj.h"

typedef struct _network_lan_obj_t {
    mp_obj_base_t base;
} network_lan_obj_t;

extern const mp_obj_type_t network_lan_type;
extern network_lan_obj_t network_lan_obj;

#endif /* MICROPY_INCLUDED_AMEBA_NETWORK_LAN_H */
