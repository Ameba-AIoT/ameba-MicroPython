/*
 * This file is part of the MicroPython project, http://micropython.org/
 * The MIT License (MIT) — Copyright (c) 2024 Realtek Corporation
 */
#include <string.h>

#include "py/runtime.h"
#include "py/mphal.h"
#include "py/mperrno.h"
#ifndef MP_ENONET
#define MP_ENONET (64) /* Machine is not on the network */
#endif
#include "shared/netutils/netutils.h"
#include "extmod/modnetwork.h"

#include "wifi_api.h"
#include "wifi_api_types.h"
#include "wifi_api_event.h"
#include "wifi_api_ext.h"
#include "lwip_netconf.h"
#include "dhcps.h"

#include "network_wlan.h"
#include "rtk_status.h"

/* ---------- Global state ------------------------------------------------- */

volatile uint8_t mp_wifi_join_status = 0; /* RTW_JOINSTATUS_UNKNOWN */

wlan_if_obj_t wlan_sta_obj = {
    .base        = { &wlan_if_type },
    .if_id       = MOD_NETWORK_STA_IF,
    .sta_active  = true,
    .ap_channel  = 6,
    .ap_security = 0, /* RTW_SECURITY_OPEN */
};
wlan_if_obj_t wlan_ap_obj = {
    .base        = { &wlan_if_type },
    .if_id       = MOD_NETWORK_AP_IF,
    .ap_channel  = 6,
    .ap_security = 0x00400004, /* RTW_SECURITY_WPA2_AES_PSK */
};

/* ---------- make_new ----------------------------------------------------- */

static mp_obj_t wlan_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 1, false);
    int if_id = (n_args > 0) ? mp_obj_get_int(args[0]) : MOD_NETWORK_STA_IF;
    if (if_id == MOD_NETWORK_STA_IF) {
        return MP_OBJ_FROM_PTR(&wlan_sta_obj);
    } else if (if_id == MOD_NETWORK_AP_IF) {
        return MP_OBJ_FROM_PTR(&wlan_ap_obj);
    }
    mp_raise_ValueError(MP_ERROR_TEXT("invalid WLAN interface"));
}

/* ---------- active() ----------------------------------------------------- */

/* Actually starts broadcasting: wifi_start_ap() + DHCP server. Requires
 * self->ap_ssid_len != 0. Called from wlan_active(True) when the SSID is
 * already configured, and from wlan_config() when a new SSID arrives while
 * active(True) is already pending (see ap_active_requested below) --
 * Realtek's wifi_start_ap() takes the full SSID/config in one call, unlike
 * esp-idf/cyw43 which let active-up and SSID-config happen independently in
 * either order, so we have to track the "wants active" intent ourselves. */
static void wlan_ap_start(wlan_if_obj_t *self) {
    /* Idempotent cleanup */
    dhcps_deinit(pnetif_ap);
    lwip_clear_ip(SOFTAP_WLAN_INDEX);
    wifi_stop_ap();

    struct rtw_softap_info ap_cfg = {0};
    memcpy(ap_cfg.ssid.val, self->ap_ssid, self->ap_ssid_len);
    ap_cfg.ssid.len      = self->ap_ssid_len;
    ap_cfg.security_type = self->ap_security;
    if (self->ap_security == RTW_SECURITY_OPEN) {
        ap_cfg.password     = NULL;
        ap_cfg.password_len = 0;
    } else {
        ap_cfg.password     = (u8 *)self->ap_key;
        ap_cfg.password_len = self->ap_key_len;
    }
    ap_cfg.channel       = self->ap_channel;
    ap_cfg.hidden_ssid   = self->ap_hidden ? 1 : 0;

    MP_THREAD_GIL_EXIT();
    s32 ap_ret = wifi_start_ap(&ap_cfg);
    MP_THREAD_GIL_ENTER();
    if (ap_ret != RTK_SUCCESS) {
        mp_raise_OSError(MP_EIO);
    }
    lwip_alloc_ip(NETIF_WLAN_AP_INDEX);
    dhcps_init(pnetif_ap);
    dhcps_start(pnetif_ap);
}

static mp_obj_t wlan_active(size_t n_args, const mp_obj_t *args) {
    wlan_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    if (self->if_id == MOD_NETWORK_STA_IF) {
        if (n_args == 1) {
            return mp_obj_new_bool(self->sta_active);
        }
        self->sta_active = mp_obj_is_true(args[1]);
        return mp_const_none;
    }

    /* AP */
    if (n_args == 1) {
        return mp_obj_new_bool(wifi_is_running(SOFTAP_WLAN_INDEX) != 0);
    }

    if (mp_obj_is_true(args[1])) {
        self->ap_active_requested = true;
        /* If the SSID isn't configured yet, defer the actual wifi_start_ap()
         * call to wlan_config() -- this supports both call orders:
         * config() then active(True) (starts here), and active(True) then
         * config() (starts once config() supplies the SSID). */
        if (self->ap_ssid_len == 0) {
            return mp_const_none;
        }
        wlan_ap_start(self);
    } else {
        self->ap_active_requested = false;
        dhcps_deinit(pnetif_ap);
        lwip_clear_ip(SOFTAP_WLAN_INDEX);
        wifi_stop_ap();
        self->ap_ssid_len = 0; /* require re-config before next active(True) */
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wlan_active_obj, 1, 2, wlan_active);

/* ---------- connect() ---------------------------------------------------- */

static mp_obj_t wlan_connect(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    wlan_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (self->if_id != MOD_NETWORK_STA_IF) {
        mp_raise_TypeError(MP_ERROR_TEXT("connect only valid for STA"));
    }

    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_ssid,     MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_key,      MP_ARG_OBJ,                   {.u_obj = mp_const_none} },
        { MP_QSTR_bssid,    MP_ARG_KW_ONLY | MP_ARG_OBJ,  {.u_obj = mp_const_none} },
        { MP_QSTR_security, MP_ARG_KW_ONLY | MP_ARG_INT,  {.u_int = 0} },
    };
    mp_arg_val_t vals[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kwargs,
        MP_ARRAY_SIZE(allowed_args), allowed_args, vals);

    struct rtw_network_info param = {0};

    size_t ssid_len;
    const char *ssid = mp_obj_str_get_data(vals[0].u_obj, &ssid_len);
    if (ssid_len > RTW_ESSID_MAX_SIZE) {
        mp_raise_ValueError(MP_ERROR_TEXT("SSID too long"));
    }
    memcpy(param.ssid.val, ssid, ssid_len);
    param.ssid.len = (u8)ssid_len;

    if (vals[1].u_obj != mp_const_none) {
        size_t key_len;
        const char *key = mp_obj_str_get_data(vals[1].u_obj, &key_len);
        if (key_len > RTW_MAX_PSK_LEN) {
            mp_raise_ValueError(MP_ERROR_TEXT("key too long"));
        }
        param.password     = (u8 *)key;
        param.password_len = (s32)key_len;
    }

    param.security_type = (u32)vals[3].u_int;

    if (vals[2].u_obj != mp_const_none) {
        size_t blen;
        const uint8_t *bssid =
            (const uint8_t *)mp_obj_str_get_data(vals[2].u_obj, &blen);
        if (blen != 6) {
            mp_raise_ValueError(MP_ERROR_TEXT("BSSID must be 6 bytes"));
        }
        memcpy(param.bssid.octet, bssid, 6);
    }

    MP_THREAD_GIL_EXIT();
    s32 ret = wifi_connect(&param, 1);
    MP_THREAD_GIL_ENTER();

    if (ret != RTK_SUCCESS) {
        if (ret == -RTK_ERR_WIFI_CONN_SCAN_FAIL) {
            mp_raise_OSError(MP_ENONET);
        } else if (ret == -RTK_ERR_WIFI_CONN_INVALID_KEY) {
            mp_raise_OSError(MP_EINVAL);
        } else if (ret == -RTK_ERR_WIFI_CONN_AUTH_PASSWORD_WRONG ||
                   ret == -RTK_ERR_WIFI_CONN_4WAY_PASSWORD_WRONG ||
                   ret == -RTK_ERR_WIFI_CONN_4WAY_HANDSHAKE_FAIL) {
            mp_raise_OSError(MP_EACCES);
        } else {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
    }

    MP_THREAD_GIL_EXIT();
    ret = lwip_request_ip(NETIF_WLAN_STA_INDEX);
    MP_THREAD_GIL_ENTER();

    if (ret != DHCP_ADDRESS_ASSIGNED) {
        wifi_disconnect();
        lwip_clear_ip(NETIF_WLAN_STA_INDEX);
        mp_raise_OSError(MP_ENONET);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(wlan_connect_obj, 1, wlan_connect);

/* ---------- disconnect() ------------------------------------------------- */

static mp_obj_t wlan_disconnect(mp_obj_t self_in) {
    wlan_if_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->if_id == MOD_NETWORK_STA_IF) {
        wifi_disconnect();
        user_static_ip.use_static_ip = 0;
        lwip_clear_ip(NETIF_WLAN_STA_INDEX);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(wlan_disconnect_obj, wlan_disconnect);

/* ---------- isconnected() ------------------------------------------------ */

static mp_obj_t wlan_isconnected(mp_obj_t self_in) {
    wlan_if_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->if_id == MOD_NETWORK_STA_IF) {
        return mp_obj_new_bool(
            lwip_check_connectivity(NETIF_WLAN_STA_INDEX) == CONNECTION_VALID);
    }
    return mp_obj_new_bool(wifi_is_running(SOFTAP_WLAN_INDEX) != 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(wlan_isconnected_obj, wlan_isconnected);

/* ---------- status() ----------------------------------------------------- */

static mp_obj_t wlan_status(size_t n_args, const mp_obj_t *args) {
    wlan_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    if (n_args == 2) {
        const char *param = mp_obj_str_get_str(args[1]);
        if (self->if_id == MOD_NETWORK_STA_IF) {
            union rtw_phy_stats stats = {0};
            wifi_get_phy_stats(STA_WLAN_INDEX, NULL, &stats);
            if (strcmp(param, "rssi") == 0) {
                return MP_OBJ_NEW_SMALL_INT((int)stats.sta.rssi);
            } else if (strcmp(param, "data_rssi") == 0) {
                return MP_OBJ_NEW_SMALL_INT((int)stats.sta.data_rssi);
            } else if (strcmp(param, "snr") == 0) {
                return MP_OBJ_NEW_SMALL_INT((int)stats.sta.snr);
            }
        } else if (strcmp(param, "stations") == 0) {
            if (self->if_id != MOD_NETWORK_AP_IF) {
                mp_raise_ValueError(MP_ERROR_TEXT("stations only valid for AP"));
            }
            struct rtw_client_list client_info = {0};
            wifi_ap_get_connected_clients(&client_info);

            uint8_t *ap_ip = lwip_get_ip(NETIF_WLAN_AP_INDEX);

            mp_obj_t result = mp_obj_new_list(0, NULL);
            for (u32 i = 0; i < client_info.count; ++i) {
                uint8_t *mac = client_info.mac_list[i].octet;

                uint8_t ip4 = dhcps_search_client_ip(pnetif_ap, mac);

                union rtw_phy_stats phy = {0};
                wifi_get_phy_stats(SOFTAP_WLAN_INDEX, mac, &phy);

                mp_obj_t dict = mp_obj_new_dict(3);
                mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_mac),
                    mp_obj_new_bytes(mac, 6));
                char ip_str[16];
                if (ip4 != 0 && ap_ip != NULL) {
                    snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                        ap_ip[0], ap_ip[1], ap_ip[2], ip4);
                } else {
                    snprintf(ip_str, sizeof(ip_str), "0.0.0.0");
                }
                mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_ip),
                    mp_obj_new_str(ip_str, strlen(ip_str)));
                mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_rssi),
                    MP_OBJ_NEW_SMALL_INT((int)phy.ap.data_rssi));
                mp_obj_list_append(result, dict);
            }
            return result;
        }
        mp_raise_ValueError(MP_ERROR_TEXT("unknown status param"));
    }

    if (self->if_id == MOD_NETWORK_STA_IF) {
        return MP_OBJ_NEW_SMALL_INT((int)mp_wifi_join_status);
    }

    /* AP: number of associated clients */
    struct rtw_client_list client_info = {0};
    wifi_ap_get_connected_clients(&client_info);
    return MP_OBJ_NEW_SMALL_INT((int)client_info.count);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wlan_status_obj, 1, 2, wlan_status);

/* ---------- scan() ------------------------------------------------------- */

static mp_obj_t wlan_scan(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_ssid,     MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_channels, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t vals[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, args + 1, kwargs,
        MP_ARRAY_SIZE(allowed_args), allowed_args, vals);

    struct rtw_scan_param scan_param = {0};
    scan_param.options = RTW_SCAN_ACTIVE;

    // Directed SSID scan: buffer lives on the stack across the GIL exit.
    u8 ssid_buf[RTW_ESSID_MAX_SIZE + 1] = {0};
    if (vals[0].u_obj != mp_const_none) {
        size_t ssid_len;
        const char *ssid_str = mp_obj_str_get_data(vals[0].u_obj, &ssid_len);
        if (ssid_len > RTW_ESSID_MAX_SIZE) {
            mp_raise_ValueError(MP_ERROR_TEXT("SSID too long"));
        }
        memcpy(ssid_buf, ssid_str, ssid_len);
        scan_param.ssid = ssid_buf;
    }

    // Channel list scan: build u8 array from Python list/tuple of ints.
    u8 ch_buf[14] = {0};
    if (vals[1].u_obj != mp_const_none) {
        size_t ch_count;
        mp_obj_t *ch_items;
        mp_obj_get_array(vals[1].u_obj, &ch_count, &ch_items);
        if (ch_count > sizeof(ch_buf)) {
            mp_raise_ValueError(MP_ERROR_TEXT("too many channels"));
        }
        for (size_t i = 0; i < ch_count; ++i) {
            ch_buf[i] = (u8)mp_obj_get_int(ch_items[i]);
        }
        scan_param.channel_list = ch_buf;
        scan_param.channel_list_num = (u8)ch_count;
    }

    MP_THREAD_GIL_EXIT();
    s32 ap_count = wifi_scan_networks(&scan_param, 1);
    MP_THREAD_GIL_ENTER();

    mp_obj_t list = mp_obj_new_list(0, NULL);
    if (ap_count <= 0) {
        return list;
    }

    struct rtw_scan_result *ap_list = m_new(struct rtw_scan_result, (size_t)ap_count);
    u32 count = (u32)ap_count;
    if (wifi_get_scan_records(&count, ap_list) < 0) {
        m_del(struct rtw_scan_result, ap_list, (size_t)ap_count);
        return list;
    }

    for (u32 i = 0; i < count; i++) {
        struct rtw_scan_result *ap = &ap_list[i];
        ap->ssid.val[ap->ssid.len] = '\0';

        mp_obj_tuple_t *t = mp_obj_new_tuple(7, NULL);
        t->items[0] = mp_obj_new_bytes((byte *)ap->ssid.val, ap->ssid.len);
        t->items[1] = mp_obj_new_bytes(ap->bssid.octet, sizeof(ap->bssid.octet));
        t->items[2] = MP_OBJ_NEW_SMALL_INT((int)ap->channel);
        t->items[3] = MP_OBJ_NEW_SMALL_INT((int)ap->signal_strength);
        t->items[4] = MP_OBJ_NEW_SMALL_INT((int)ap->security);
        t->items[5] = mp_obj_new_bool(ap->ssid.len == 0);
        t->items[6] = MP_OBJ_NEW_SMALL_INT((int)ap->wireless_mode);
        mp_obj_list_append(list, MP_OBJ_FROM_PTR(t));
    }
    m_del(struct rtw_scan_result, ap_list, (size_t)ap_count);
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(wlan_scan_obj, 1, wlan_scan);

/* ---------- ifconfig() --------------------------------------------------- */

static mp_obj_t wlan_ifconfig(size_t n_args, const mp_obj_t *args) {
    wlan_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    uint8_t idx = (self->if_id == MOD_NETWORK_STA_IF)
                  ? NETIF_WLAN_STA_INDEX : NETIF_WLAN_AP_INDEX;

    if (n_args == 1) {
        uint8_t *ip  = lwip_get_ip(idx);
        uint8_t *msk = lwip_get_mask(idx);
        uint8_t *gw  = lwip_get_gw(idx);

        mp_obj_t tuple[4];
        tuple[0] = netutils_format_ipv4_addr(ip,  NETUTILS_BIG);
        tuple[1] = netutils_format_ipv4_addr(msk, NETUTILS_BIG);
        tuple[2] = netutils_format_ipv4_addr(gw,  NETUTILS_BIG);
        #if LWIP_DNS
        struct ip_addr dns_addr = {0};
        lwip_get_dns(&dns_addr);
        tuple[3] = netutils_format_ipv4_addr((uint8_t *)&dns_addr.addr, NETUTILS_BIG);
        #else
        tuple[3] = mp_obj_new_str("0.0.0.0", 7);
        #endif
        return mp_obj_new_tuple(4, tuple);
    }

    /* setter */
    mp_obj_t *items;
    mp_obj_get_array_fixed_n(args[1], 4, &items);

    uint32_t ip_addr = 0, netmask = 0, gw_addr = 0;
    netutils_parse_ipv4_addr(items[0], (void *)&ip_addr, NETUTILS_BIG);
    netutils_parse_ipv4_addr(items[1], (void *)&netmask, NETUTILS_BIG);
    netutils_parse_ipv4_addr(items[2], (void *)&gw_addr, NETUTILS_BIG);

    /* lwip_set_ip() applies PP_HTONL internally, so convert from the
     * byte-array representation (NETUTILS_BIG) to host-byte-order first. */
    uint32_t ip_h  = PP_HTONL(ip_addr);
    uint32_t msk_h = PP_HTONL(netmask);
    uint32_t gw_h  = PP_HTONL(gw_addr);

    if (self->if_id == MOD_NETWORK_STA_IF) {
        user_static_ip.use_static_ip = 1;
        user_static_ip.addr    = ip_h;
        user_static_ip.netmask = msk_h;
        user_static_ip.gw      = gw_h;
        /* Apply immediately so the getter reflects the new value at once. */
        lwip_set_ip(NETIF_WLAN_STA_INDEX, ip_h, msk_h, gw_h);
    } else {
        dhcps_deinit(pnetif_ap);
        lwip_set_ip(NETIF_WLAN_AP_INDEX, ip_h, msk_h, gw_h);
        dhcps_init(pnetif_ap);
        dhcps_start(pnetif_ap);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(wlan_ifconfig_obj, 1, 2, wlan_ifconfig);

/* ---------- config() ----------------------------------------------------- */

static mp_obj_t wlan_config(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    wlan_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    if (kwargs->used == 0) {
        /* getter */
        if (n_args != 2) {
            mp_raise_TypeError(MP_ERROR_TEXT("config() query takes one parameter name"));
        }
        const char *key = mp_obj_str_get_str(args[1]);
        struct rtw_wifi_setting setting = {0};

        if (strcmp(key, "ssid") == 0 || strcmp(key, "essid") == 0) {
            if (self->if_id == MOD_NETWORK_AP_IF) {
                return mp_obj_new_str(self->ap_ssid, self->ap_ssid_len);
            }
            wifi_get_setting(STA_WLAN_INDEX, &setting);
            return mp_obj_new_str((const char *)setting.ssid,
                strlen((const char *)setting.ssid));
        }
        if (strcmp(key, "channel") == 0) {
            if (self->if_id == MOD_NETWORK_AP_IF) {
                return MP_OBJ_NEW_SMALL_INT(self->ap_channel);
            }
            wifi_get_setting(STA_WLAN_INDEX, &setting);
            return MP_OBJ_NEW_SMALL_INT(setting.channel);
        }
        if (strcmp(key, "mac") == 0) {
            uint8_t *mac = lwip_get_mac(
                self->if_id == MOD_NETWORK_STA_IF
                ? NETIF_WLAN_STA_INDEX : NETIF_WLAN_AP_INDEX);
            return mp_obj_new_bytes(mac, 6);
        }
        if (strcmp(key, "security") == 0) {
            if (self->if_id == MOD_NETWORK_AP_IF) {
                return MP_OBJ_NEW_SMALL_INT((int)self->ap_security);
            }
            wifi_get_setting(STA_WLAN_INDEX, &setting);
            return MP_OBJ_NEW_SMALL_INT((int)setting.security_type);
        }
        mp_raise_ValueError(MP_ERROR_TEXT("unknown config param"));
    }

    /* setter — AP only */
    if (self->if_id != MOD_NETWORK_AP_IF) {
        mp_raise_TypeError(MP_ERROR_TEXT("config() setter only valid for AP"));
    }
    for (size_t i = 0; i < kwargs->alloc; i++) {
        if (!MP_MAP_SLOT_IS_FILLED(kwargs, i)) {
            continue;
        }
        mp_obj_t k = kwargs->table[i].key;
        mp_obj_t v = kwargs->table[i].value;
        if (k == MP_OBJ_NEW_QSTR(MP_QSTR_ssid) || k == MP_OBJ_NEW_QSTR(MP_QSTR_essid)) {
            size_t len;
            const char *s = mp_obj_str_get_data(v, &len);
            if (len > RTW_ESSID_MAX_SIZE) {
                mp_raise_ValueError(MP_ERROR_TEXT("SSID too long"));
            }
            memcpy(self->ap_ssid, s, len);
            self->ap_ssid[len]  = '\0';
            self->ap_ssid_len   = (uint8_t)len;
        } else if (k == MP_OBJ_NEW_QSTR(MP_QSTR_key) || k == MP_OBJ_NEW_QSTR(MP_QSTR_password)) {
            size_t len;
            const char *s = mp_obj_str_get_data(v, &len);
            if (len > RTW_MAX_PSK_LEN) {
                mp_raise_ValueError(MP_ERROR_TEXT("key too long"));
            }
            memcpy(self->ap_key, s, len);
            self->ap_key[len]   = '\0';
            self->ap_key_len    = (uint8_t)len;
        } else if (k == MP_OBJ_NEW_QSTR(MP_QSTR_channel)) {
            self->ap_channel    = (uint8_t)mp_obj_get_int(v);
        } else if (k == MP_OBJ_NEW_QSTR(MP_QSTR_security)) {
            self->ap_security   = (uint32_t)mp_obj_get_int(v);
        } else if (k == MP_OBJ_NEW_QSTR(MP_QSTR_hidden)) {
            self->ap_hidden     = mp_obj_is_true(v);
        } else {
            mp_raise_msg_varg(&mp_type_TypeError,
                MP_ERROR_TEXT("unexpected keyword argument '%s'"),
                qstr_str(MP_OBJ_QSTR_VALUE(k)));
        }
    }
    /* active(True) was called before the SSID was known -- now that config()
     * has supplied one, actually start broadcasting. */
    if (self->ap_active_requested && self->ap_ssid_len != 0 &&
        !wifi_is_running(SOFTAP_WLAN_INDEX)) {
        wlan_ap_start(self);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(wlan_config_obj, 1, wlan_config);

/* ---------- locals_dict -------------------------------------------------- */

static const mp_rom_map_elem_t wlan_if_locals_dict_table[] = {
    /* Methods */
    { MP_ROM_QSTR(MP_QSTR_active),      MP_ROM_PTR(&wlan_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect),     MP_ROM_PTR(&wlan_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect),  MP_ROM_PTR(&wlan_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_isconnected), MP_ROM_PTR(&wlan_isconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_status),      MP_ROM_PTR(&wlan_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_scan),        MP_ROM_PTR(&wlan_scan_obj) },
    { MP_ROM_QSTR(MP_QSTR_ifconfig),    MP_ROM_PTR(&wlan_ifconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_config),      MP_ROM_PTR(&wlan_config_obj) },

    /* Wireless mode constants (bitmask, from enum rtw_wireless_mode in wifi_api_types.h) */
    { MP_ROM_QSTR(MP_QSTR_MODE_11B),    MP_ROM_INT(RTW_80211_B) },
    { MP_ROM_QSTR(MP_QSTR_MODE_11G),    MP_ROM_INT(RTW_80211_G) },
    { MP_ROM_QSTR(MP_QSTR_MODE_11N),    MP_ROM_INT(RTW_80211_N) },
    { MP_ROM_QSTR(MP_QSTR_MODE_11A),    MP_ROM_INT(RTW_80211_A) },
    { MP_ROM_QSTR(MP_QSTR_MODE_11AC),   MP_ROM_INT(RTW_80211_AC) },
    { MP_ROM_QSTR(MP_QSTR_MODE_11AX),   MP_ROM_INT(RTW_80211_AX) },

    /* Interface constants (aliases of module-level network.STA_IF/AP_IF, mirroring
       esp32: both forms share the same value so old and new code interoperate) */
    { MP_ROM_QSTR(MP_QSTR_IF_STA),      MP_ROM_INT(MOD_NETWORK_STA_IF) },
    { MP_ROM_QSTR(MP_QSTR_IF_AP),       MP_ROM_INT(MOD_NETWORK_AP_IF) },

    /* Security constants */
    { MP_ROM_QSTR(MP_QSTR_SEC_OPEN),        MP_ROM_INT(RTW_SECURITY_OPEN) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WEP),         MP_ROM_INT(RTW_SECURITY_WEP_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA),         MP_ROM_INT(RTW_SECURITY_WPA_AES_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA2),        MP_ROM_INT(RTW_SECURITY_WPA2_AES_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA_WPA2),    MP_ROM_INT(RTW_SECURITY_WPA_WPA2_AES_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA3),         MP_ROM_INT(RTW_SECURITY_WPA3_AES_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA2_WPA3),    MP_ROM_INT(RTW_SECURITY_WPA2_WPA3_MIXED) },
    { MP_ROM_QSTR(MP_QSTR_SEC_OWE),          MP_ROM_INT(RTW_SECURITY_WPA3_OWE) },

    /* Status constants (RTW_JOINSTATUS_* values) */
    { MP_ROM_QSTR(MP_QSTR_STAT_IDLE),              MP_ROM_INT(0) },  /* RTW_JOINSTATUS_UNKNOWN */
    { MP_ROM_QSTR(MP_QSTR_STAT_CONNECTING),        MP_ROM_INT(2) },  /* RTW_JOINSTATUS_SCANNING */
    { MP_ROM_QSTR(MP_QSTR_STAT_GOT_IP),            MP_ROM_INT(9) },  /* RTW_JOINSTATUS_SUCCESS */
    { MP_ROM_QSTR(MP_QSTR_STAT_WRONG_PASSWORD),    MP_ROM_INT(10) }, /* RTW_JOINSTATUS_FAIL */
    { MP_ROM_QSTR(MP_QSTR_STAT_HANDSHAKE_TIMEOUT), MP_ROM_INT(11) }, /* RTW_JOINSTATUS_DISCONNECT */
};
static MP_DEFINE_CONST_DICT(wlan_if_locals_dict, wlan_if_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    wlan_if_type,
    MP_QSTR_WLAN,
    MP_TYPE_FLAG_NONE,
    make_new, wlan_make_new,
    locals_dict, &wlan_if_locals_dict
);
