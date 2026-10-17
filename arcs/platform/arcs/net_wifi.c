/*
 * Wi-Fi station for the gadget: one saved network, auto-connect, DHCP,
 * and the lwIP netdev registered for BSD sockets.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"

#include "net_al.h"
#include "net_def.h"
#include "lisa_wifi.h"
#include "mac_manager.h"
#include "mac_manager_ops.h"
#include "wifi_manager/wifi_manager.h"
#include "wifi_api.h"
#include "netdev.h"
#include "shell.h"

#include "platform_arcs.h"

#define TAG "plat_wifi"
#include "lisa_log.h"

#define AUTO_CONNECT_INTERVAL_MS 3000

static struct {
    mac_manager_t *mac;
    bool ready;
    bool associated;
    bool netdev_registered;
    volatile uint32_t ip;   /* network byte order, 0 = none */
    int last_reason;
} s_wifi;

static void dhcp_status_cb(int vif_idx, bool success, uint32_t ip, uint32_t mask, uint32_t gw, void *arg)
{
    (void)mask; (void)arg;
    if (vif_idx != WIFI_VIF_STA_IDX) {
        return;
    }
    if (success) {
        s_wifi.ip = ip;
        LISA_LOGI(TAG, "got ip %u.%u.%u.%u gw %u.%u.%u.%u",
                  ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, ip >> 24,
                  gw & 0xff, (gw >> 8) & 0xff, (gw >> 16) & 0xff, gw >> 24);
    } else {
        s_wifi.ip = 0;
        LISA_LOGW(TAG, "dhcp failed");
    }
}

static void connection_cb(wifi_mgr_connection_info_t *info, void *arg)
{
    (void)arg;
    switch (info->status) {
    case WIFI_MGR_STA_CONNECTED: {
        net_if_t *nif = net_if_get(WIFI_VIF_STA_IDX);
        s_wifi.associated = true;
        LISA_LOGI(TAG, "associated with %s ap %s ch %d", info->sta_info ? info->sta_info->ssid : "?",
                  info->sta_info ? info->sta_info->bssid : "?", info->sta_info ? info->sta_info->channel : 0);
        net_if_up(nif);
        if (!nif->static_ip) {
            ls_dhcpc_start(WIFI_VIF_STA_IDX);
        }
        break;
    }
    case WIFI_MGR_STA_DISCONNECTED:
    case WIFI_MGR_STA_CONNECT_FAILED:
        s_wifi.associated = false;
        s_wifi.ip = 0;
        s_wifi.last_reason = info->reason;
        LISA_LOGW(TAG, "disconnected status=%d error=%d reason=%d",
                  info->status, info->error_code, info->reason);
        ls_dhcpc_stop(WIFI_VIF_STA_IDX);
        net_if_down(net_if_get(WIFI_VIF_STA_IDX));
        break;
    case WIFI_MGR_STA_CONNECTING:
        LISA_LOGI(TAG, "connecting");
        break;
    default:
        break;
    }
}

static void save_build_time_network(void)
{
    if (CONFIG_GADGET_WIFI_SSID[0] == '\0') {
        return;
    }
    wifi_mgr_sta_config_t cfg = {0};
    strncpy(cfg.ssid, CONFIG_GADGET_WIFI_SSID, sizeof(cfg.ssid) - 1);
    strncpy(cfg.pwd, CONFIG_GADGET_WIFI_PASSWORD, sizeof(cfg.pwd) - 1);
    cfg.encryption_mode = WIFI_MGR_WIFI_AUTH_AUTO;

    wifi_mgr_sta_config_t saved = {0};
    int found = wifi_mgr_storage_search_ap(&saved, 1, SEARCH_BY_SSID, cfg.ssid);
    if (found > 0 && strcmp(saved.pwd, cfg.pwd) == 0) {
        return;
    }
    int ret = wifi_mgr_storage_save_ap(&cfg);
    LISA_LOGI(TAG, "saved build-time network %s: %d", cfg.ssid, ret);
}

/*
 * The SDK's auto-connect picks the strongest router of the saved network, then
 * connects to that one router (its BSSID) on its channel. The Wi-Fi firmware
 * takes the BSSID as a hard limit (wpa_supplicant "SET_NETWORK bssid") and scans
 * only that channel, so its roaming (bgscan: below -68 dBm, scan every 10 s and
 * move to a better router) can never move to another router of a mesh: the dog
 * stays on the far one until the link is lost. Connect by SSID only instead;
 * the firmware then picks the router and roams between them.
 */
static wifi_manager_wifi_ops_t s_wifi_ops;
static int (*s_sta_connect)(wifi_mgr_wifi_sta_config_t *cfg, uint32_t timeout_ms);

static int sta_connect_any_router(wifi_mgr_wifi_sta_config_t *cfg, uint32_t timeout_ms)
{
    wifi_mgr_wifi_sta_config_t any = *cfg;
    any.bssid[0] = '\0';
    any.channel = 0;
    return s_sta_connect(&any, timeout_ms);
}

static void wifi_init_done(void)
{
    wifi_mgr_ops_t *ops = wifi_mgr_ops_get();
    s_wifi_ops = *ops->wifi_ops;
    s_sta_connect = s_wifi_ops.sta_connect;
    s_wifi_ops.sta_connect = sta_connect_any_router;
    ops->wifi_ops = &s_wifi_ops;
    wifi_mgr_init(ops);
    wifi_mgr_sta_enable();
    wifi_mgr_sta_add_connection_cb(connection_cb, NULL);
    s_wifi.ready = true;

    if (!s_wifi.netdev_registered && app_netdev_register("wifi0", 200) == 0) {
        s_wifi.netdev_registered = true;
    }

    save_build_time_network();
    wifi_mgr_autoconn_config_t autoconn = { .interval_ms = AUTO_CONNECT_INTERVAL_MS };
    wifi_mgr_auto_connect_start(&autoconn);
    LISA_LOGI(TAG, "station started");
}

int plat_wifi_power_save(bool on)
{
    ls_err_t r = wifi_ps_mode_set(on ? WIFI_PS_MODE_DTIM : WIFI_PS_MODE_OFF);
    LISA_LOGI(TAG, "power save %s: %d", on ? "on" : "off", (int)r);
    return r == 0 ? 0 : -1;
}

/* The last router changes, for "wifi": a roaming test away from USB leaves no log. */
#define ROUTER_HISTORY 8
static struct {
    uint32_t at_s;
    uint8_t bssid[6];
    int8_t channel, rssi;
    bool roamed;
} s_routers[ROUTER_HISTORY];
static unsigned s_router_count;

void plat_wifi_watch_router(void)
{
    static uint8_t last[6];
    struct wifi_link_status ls;
    memset(&ls, 0, sizeof(ls));
    if (!s_wifi.associated || wifi_get_link_status(&ls) != LS_OK) {
        memset(last, 0, sizeof(last));
        return;
    }
    if (memcmp(last, ls.bssid, 6) != 0) {
        LISA_LOGI(TAG, "%s ap %02x:%02x:%02x:%02x:%02x:%02x ch %d rssi %d dBm",
                  last[0] | last[1] | last[2] | last[3] | last[4] | last[5] ? "roamed to" : "on",
                  ls.bssid[0], ls.bssid[1], ls.bssid[2], ls.bssid[3], ls.bssid[4], ls.bssid[5], ls.channel, ls.rssi);
        unsigned k = s_router_count++ % ROUTER_HISTORY;
        s_routers[k].at_s = (uint32_t)(plat_uptime_ms() / 1000);
        memcpy(s_routers[k].bssid, ls.bssid, 6);
        s_routers[k].channel = ls.channel;
        s_routers[k].rssi = ls.rssi;
        s_routers[k].roamed = (last[0] | last[1] | last[2] | last[3] | last[4] | last[5]) != 0;
        memcpy(last, ls.bssid, 6);
    }
}

static int8_t custom_mac(uint8_t mac[6])
{
    return mac ? mac_manager_get(s_wifi.mac, mac, 6) : -1;
}

int plat_wifi_start(void)
{
    mac_manager_config_t mac_cfg = { .random_mac_if_mac_invalid = false };
    s_wifi.mac = mac_manager_init(mac_manager_ops_get()->mem_ops, mac_manager_ops_get()->content_ops, &mac_cfg);
    if (s_wifi.mac == NULL) {
        LISA_LOGE(TAG, "mac manager init failed");
        return -1;
    }
    net_dhcp_register_status_callback(dhcp_status_cb, NULL);
    lisa_wifi_ops_t ops = { .init_done = wifi_init_done, .custom_mac = custom_mac };
    lisa_wifi_init(&ops);
    return 0;
}

bool plat_wifi_has_ip(void)
{
    return s_wifi.ip != 0;
}

int plat_wifi_get_mac(uint8_t mac[6])
{
    return custom_mac(mac) >= 0 ? 0 : -1;
}

int plat_wifi_get_ssid(char *out, size_t cap)
{
    wifi_mgr_sta_config_t cur = {0};
    out[0] = '\0';
    if (s_wifi.ip == 0 || wifi_mgr_sta_get_connected_info(&cur) != 0) {
        return -1;
    }
    snprintf(out, cap, "%.*s", 32, cur.ssid);
    return 0;
}

int plat_wifi_scan(plat_wifi_ap_t *aps, int max)
{
    enum { SCAN_MAX = 24 };
    wifi_mgr_scan_info_t *raw = calloc(SCAN_MAX, sizeof(*raw));
    if (raw == NULL || !s_wifi.ready) {
        free(raw);
        return 0;
    }
    int n = wifi_mgr_scan_ap(raw, SCAN_MAX, false);
    int out = 0;
    for (int i = 0; i < n && out < max; i++) {
        char ssid[33];
        memcpy(ssid, raw[i].ssid, 32);
        ssid[32] = '\0';
        if (ssid[0] == '\0') {
            continue;  /* hidden network */
        }
        bool dup = false;
        for (int j = 0; j < out; j++) {
            if (strcmp(aps[j].ssid, ssid) == 0) {
                dup = true;
                if (raw[i].rssi > aps[j].rssi) {
                    aps[j].rssi = raw[i].rssi;
                }
                break;
            }
        }
        if (dup) {
            continue;
        }
        strcpy(aps[out].ssid, ssid);
        aps[out].rssi = raw[i].rssi;
        aps[out].secure = raw[i].encryption_mode != WIFI_MGR_WIFI_AUTH_OPEN;
        out++;
    }
    free(raw);
    LISA_LOGI(TAG, "scan: %d networks (%d raw)", out, n);
    return out;
}

int plat_wifi_join(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (!s_wifi.ready || strlen(ssid) > 32 || strlen(password) > 63) {
        return -1;
    }
    wifi_mgr_sta_config_t cur = {0};
    if (s_wifi.ip != 0 && wifi_mgr_sta_get_connected_info(&cur) == 0 && strncmp(cur.ssid, ssid, 32) == 0 &&
        strcmp(cur.pwd, password) == 0) {
        LISA_LOGI(TAG, "join %s: already connected", ssid);
        return 0;
    }

    wifi_mgr_auto_connect_stop();
    if (s_wifi.associated) {
        wifi_mgr_sta_disconnect(false);
    }
    s_wifi.ip = 0;
    wifi_mgr_sta_config_t cfg = {0};
    strncpy(cfg.ssid, ssid, sizeof(cfg.ssid));
    strncpy(cfg.pwd, password, sizeof(cfg.pwd) - 1);
    cfg.encryption_mode = WIFI_MGR_WIFI_AUTH_AUTO;
    int ret = wifi_mgr_sta_connect(&cfg, false);
    LISA_LOGI(TAG, "join %s: connect %d", ssid, ret);
    uint32_t waited = 0;
    while (ret == 0 && s_wifi.ip == 0 && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    bool ok = s_wifi.ip != 0;
    if (ok) {
        wifi_mgr_storage_save_ap(&cfg);
    }
    wifi_mgr_autoconn_config_t autoconn = { .interval_ms = AUTO_CONNECT_INTERVAL_MS };
    wifi_mgr_auto_connect_start(&autoconn);
    LISA_LOGI(TAG, "join %s: %s", ssid, ok ? "ok" : "failed");
    return ok ? 0 : -1;
}

static int wifi_cmd(int argc, char **argv)
{
    uint32_t ip = s_wifi.ip;
    if (argc >= 4 && strcmp(argv[1], "connect") == 0) {
        wifi_mgr_sta_config_t cfg = {0};
        strncpy(cfg.ssid, argv[2], sizeof(cfg.ssid) - 1);
        strncpy(cfg.pwd, argv[3], sizeof(cfg.pwd) - 1);
        cfg.encryption_mode = WIFI_MGR_WIFI_AUTH_AUTO;
        wifi_mgr_storage_save_ap(&cfg);
        shellPrint(shellGetCurrent(), "connect %s: %d\n", cfg.ssid, wifi_mgr_sta_connect(&cfg, true));
        return 0;
    }
    shellPrint(shellGetCurrent(), "ready=%d associated=%d ip=%u.%u.%u.%u last_reason=%d\n",
           s_wifi.ready, s_wifi.associated,
           ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, ip >> 24, s_wifi.last_reason);
    /* Which router (access point) of the network, and how well it is heard. */
    struct wifi_link_status ls;
    memset(&ls, 0, sizeof(ls));
    if (s_wifi.associated && wifi_get_link_status(&ls) == LS_OK) {
        shellPrint(shellGetCurrent(), "ap %02x:%02x:%02x:%02x:%02x:%02x ch %d rssi %d dBm\n", ls.bssid[0],
                   ls.bssid[1], ls.bssid[2], ls.bssid[3], ls.bssid[4], ls.bssid[5], ls.channel, ls.rssi);
    }
    /* "on": joined after a disconnect or boot; "roamed": moved without losing the network. */
    unsigned first = s_router_count > ROUTER_HISTORY ? s_router_count - ROUTER_HISTORY : 0;
    for (unsigned i = first; i < s_router_count; i++) {
        unsigned k = i % ROUTER_HISTORY;
        const uint8_t *b = s_routers[k].bssid;
        shellPrint(shellGetCurrent(), "  %6us %-6s %02x:%02x:%02x:%02x:%02x:%02x ch %d %d dBm\n",
                   (unsigned)s_routers[k].at_s, s_routers[k].roamed ? "roamed" : "on", b[0], b[1], b[2], b[3], b[4],
                   b[5], s_routers[k].channel, s_routers[k].rssi);
    }
    if (argc >= 2 && strcmp(argv[1], "scan") == 0 && s_wifi.ready) {
        /* Every access point in range, so the routers of one network show separately. */
        enum { SCAN_MAX = 24 };
        wifi_mgr_scan_info_t *raw = calloc(SCAN_MAX, sizeof(*raw));
        int n = raw != NULL ? wifi_mgr_scan_ap(raw, SCAN_MAX, false) : 0;
        for (int i = 0; i < n; i++) {
            shellPrint(shellGetCurrent(), "  %-32.32s %s ch %2d %4d dBm\n", raw[i].ssid, raw[i].bssid,
                       raw[i].channel, raw[i].rssi);
        }
        free(raw);
    }
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 wifi, wifi_cmd, wifi [scan | connect <ssid> <password>]);
