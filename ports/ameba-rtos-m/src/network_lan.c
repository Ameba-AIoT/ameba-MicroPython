/*
 * This file is part of the MicroPython project, http://micropython.org/
 * The MIT License (MIT) — Copyright (c) 2024 Realtek Corporation
 */
#include <string.h>

#include "py/runtime.h"
#include "py/mphal.h"
#include "shared/netutils/netutils.h"
#include "extmod/modnetwork.h"

#include "ethernet.h"
#include "lwip_netconf.h"

#include "network_lan.h"

#if MICROPY_PY_NETWORK_LAN

/* Not exposed via any header (file-scope global in ethernet.c, not
 * static) -- same manual-extern pattern atcmd_ethernet.c already uses for
 * pnetif_eth. */
extern volatile int eth_link_is_up;

/* ---------- global singleton -------------------------------------------- */

network_lan_obj_t network_lan_obj = {
    .base = { &network_lan_type },
};

/* Guards eth_init() so it only ever runs once -- the vendor SDK has no
 * eth_deinit(), so calling it a second time (e.g. if a user script
 * constructs LAN(0) again after a soft reset) would stack a second set of
 * FreeRTOS tasks/semaphores on top of the still-running first one. */
static bool network_lan_eth_init_done = false;

/* ---------- make_new ----------------------------------------------------- */

static mp_obj_t network_lan_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    enum { ARG_id, ARG_phy_type, ARG_phy_addr, ARG_ref_clk_mode };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_id,           MP_ARG_REQUIRED | MP_ARG_INT, {0} },
        { MP_QSTR_phy_type,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_phy_addr,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_ref_clk_mode, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };

    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, args, &kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    if (parsed[ARG_id].u_int != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("LAN(id) must be 0 -- this board has one Ethernet port"));
    }

    /* phy_type/phy_addr/ref_clk_mode are hardcoded on this hardware (PHY is
     * RTL8201FR at MDIO address 0x01, driven by CONFIG_MAC_OUTPUT_25M --
     * see spec docs/superpowers/specs/2026-07-13-p2-lan.md). These kwargs
     * are accepted for API compatibility but only validated, never used to
     * change hardware behavior. -1 / None means "not specified", which is
     * always accepted (matches the board default). */
    if (parsed[ARG_phy_type].u_int != -1) {
        mp_raise_ValueError(MP_ERROR_TEXT("phy_type is fixed on this board"));
    }
    if (parsed[ARG_phy_addr].u_int != -1) {
        mp_raise_ValueError(MP_ERROR_TEXT("phy_addr is fixed on this board"));
    }
    if (parsed[ARG_ref_clk_mode].u_obj != mp_const_none) {
        mp_raise_ValueError(MP_ERROR_TEXT("ref_clk_mode is fixed on this board"));
    }

    /* eth_init() is fire-and-forget (see Global Constraints) -- there is no
     * public API to wait for the background ETH_INIT task to finish, and
     * the vendor's own example (example_ethernet_basic.c) doesn't wait
     * either. Only ever call it once: the SDK has no eth_deinit(), so a
     * second call would stack duplicate FreeRTOS tasks/semaphores on top
     * of the still-running first one. */
    if (!network_lan_eth_init_done) {
        eth_init();
        network_lan_eth_init_done = true;
    }

    return MP_OBJ_FROM_PTR(&network_lan_obj);
}

/* ---------- active() ------------------------------------------------------ */

static mp_obj_t network_lan_active(size_t n_args, const mp_obj_t *args) {
    if (n_args == 1) {
        return mp_obj_new_bool(netif_is_up(pnetif_eth));
    }
    if (mp_obj_is_true(args[1])) {
        lwip_netif_set_up(NETIF_ETH_INDEX);
    } else {
        lwip_netif_set_down(NETIF_ETH_INDEX);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_lan_active_obj, 1, 2, network_lan_active);

/* ---------- isconnected() -------------------------------------------------- */

static mp_obj_t network_lan_isconnected(mp_obj_t self_in) {
    (void)self_in;
    return mp_obj_new_bool(eth_link_is_up);
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_lan_isconnected_obj, network_lan_isconnected);

/* ---------- status() ------------------------------------------------------- */

static mp_obj_t network_lan_status(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    (void)args;
    /* Bare link-state integer -- matches mimxrt's network_lan_status(),
     * which returns eth_link_status(self->eth) directly. No richer status
     * enum exists on this hardware; isconnected() is the same source cast
     * to bool. */
    return MP_OBJ_NEW_SMALL_INT(eth_link_is_up);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_lan_status_obj, 1, 2, network_lan_status);

/* ---------- ifconfig() ----------------------------------------------------- */

static mp_obj_t network_lan_ifconfig(size_t n_args, const mp_obj_t *args) {
    if (n_args == 1) {
        uint8_t *ip  = lwip_get_ip(NETIF_ETH_INDEX);
        uint8_t *msk = lwip_get_mask(NETIF_ETH_INDEX);
        uint8_t *gw  = lwip_get_gw(NETIF_ETH_INDEX);

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

    mp_obj_t *items;
    mp_obj_get_array_fixed_n(args[1], 4, &items);

    uint32_t ip_addr = 0, netmask = 0, gw_addr = 0;
    netutils_parse_ipv4_addr(items[0], (void *)&ip_addr, NETUTILS_BIG);
    netutils_parse_ipv4_addr(items[1], (void *)&netmask, NETUTILS_BIG);
    netutils_parse_ipv4_addr(items[2], (void *)&gw_addr, NETUTILS_BIG);

    /* lwip_set_ip() applies PP_HTONL internally, so convert from the
     * byte-array representation (NETUTILS_BIG) to host-byte-order first
     * -- matches wlan_ifconfig()'s existing pattern in network_wlan.c. */
    uint32_t ip_h  = PP_HTONL(ip_addr);
    uint32_t msk_h = PP_HTONL(netmask);
    uint32_t gw_h  = PP_HTONL(gw_addr);

    lwip_set_ip(NETIF_ETH_INDEX, ip_h, msk_h, gw_h);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_lan_ifconfig_obj, 1, 2, network_lan_ifconfig);

/* ---------- config() ------------------------------------------------------- */

static mp_obj_t network_lan_config(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    if (kwargs->used != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("no settable config parameters on this board"));
    }
    if (n_args != 2) {
        mp_raise_TypeError(MP_ERROR_TEXT("config() query takes one parameter name"));
    }
    const char *key = mp_obj_str_get_str(args[1]);
    if (strcmp(key, "mac") != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("only 'mac' is supported"));
    }
    uint8_t *mac = lwip_get_mac(NETIF_ETH_INDEX);
    return mp_obj_new_bytes(mac, 6);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(network_lan_config_obj, 1, network_lan_config);

/* ---------- locals_dict -------------------------------------------------- */

static const mp_rom_map_elem_t network_lan_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_active),      MP_ROM_PTR(&network_lan_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_isconnected), MP_ROM_PTR(&network_lan_isconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_status),      MP_ROM_PTR(&network_lan_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_ifconfig),    MP_ROM_PTR(&network_lan_ifconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_config),      MP_ROM_PTR(&network_lan_config_obj) },
};
static MP_DEFINE_CONST_DICT(network_lan_locals_dict, network_lan_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    network_lan_type,
    MP_QSTR_LAN,
    MP_TYPE_FLAG_NONE,
    make_new, network_lan_make_new,
    locals_dict, &network_lan_locals_dict
    );

#endif // MICROPY_PY_NETWORK_LAN
