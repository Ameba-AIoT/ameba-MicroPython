/*
 * This file is part of the MicroPython project, http://micropython.org/
 * The MIT License (MIT) — Copyright (c) 2024 Realtek Corporation
 */
#ifndef MICROPY_INCLUDED_AMEBA_NETWORK_WLAN_H
#define MICROPY_INCLUDED_AMEBA_NETWORK_WLAN_H

#include "py/obj.h"
#include "extmod/modnetwork.h"

/* Join status updated by mp_wificfg.c event handler, read by network_wlan.c */
extern volatile uint8_t mp_wifi_join_status;

typedef struct _wlan_if_obj_t {
    mp_obj_base_t base;
    int if_id;           /* MOD_NETWORK_STA_IF or MOD_NETWORK_AP_IF */
    bool sta_active;     /* STA only: tracks user-visible active state */
    /* AP config (used when active(True) is called) */
    char    ap_ssid[33];
    uint8_t ap_ssid_len;
    char    ap_key[129]; /* RTW_MAX_PSK_LEN=128 + NUL */
    uint8_t ap_key_len;
    uint8_t ap_channel;
    uint32_t ap_security;
    bool    ap_hidden;
} wlan_if_obj_t;

extern const mp_obj_type_t wlan_if_type;
extern wlan_if_obj_t wlan_sta_obj;
extern wlan_if_obj_t wlan_ap_obj;

#endif /* MICROPY_INCLUDED_AMEBA_NETWORK_WLAN_H */
