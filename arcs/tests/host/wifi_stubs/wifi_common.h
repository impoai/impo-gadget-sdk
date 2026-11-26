#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef int ls_err_t;
#define LS_OK 0
#define WIFI_VIF_STA_IDX 0
#define WIFI_MGR_STA_CONNECTED 1
#define WIFI_MGR_STA_DISCONNECTED 2
#define WIFI_MGR_STA_CONNECT_FAILED 3
#define WIFI_MGR_STA_CONNECTING 4
#define WIFI_MGR_WIFI_AUTH_AUTO 0
#define WIFI_MGR_WIFI_AUTH_OPEN 1
#define WIFI_PS_MODE_DTIM 1
#define WIFI_PS_MODE_OFF 0
#define SEARCH_BY_SSID 1
#define CONFIG_GADGET_WIFI_SSID ""
#define CONFIG_GADGET_WIFI_PASSWORD ""
typedef struct { char ssid[33],pwd[64],bssid[18];int channel,rssi,encryption_mode; } wifi_mgr_sta_config_t;
typedef wifi_mgr_sta_config_t wifi_mgr_wifi_sta_config_t;
typedef wifi_mgr_sta_config_t wifi_mgr_scan_info_t;
typedef struct {int status,reason,error_code;wifi_mgr_sta_config_t *sta_info;} wifi_mgr_connection_info_t;
typedef struct { unsigned interval_ms; } wifi_mgr_autoconn_config_t;
typedef struct {int (*sta_connect)(wifi_mgr_wifi_sta_config_t *,uint32_t);} wifi_manager_wifi_ops_t;
typedef struct {wifi_manager_wifi_ops_t *wifi_ops;} wifi_mgr_ops_t;
typedef struct {bool static_ip;} net_if_t;
typedef struct {int id;} mac_manager_t;
typedef struct {bool random_mac_if_mac_invalid;} mac_manager_config_t;
typedef struct {void *mem_ops,*content_ops;} test_mac_ops_t;
typedef struct {void (*init_done)(void);int8_t (*custom_mac)(uint8_t *);} lisa_wifi_ops_t;
struct wifi_link_status {uint8_t bssid[6];int8_t channel,rssi;};
int lisa_kv_get_string(const char *,char **);
int lisa_kv_set_string(const char *,const char *);
void lisa_kv_free(void *);
int wifi_mgr_storage_search_ap(wifi_mgr_sta_config_t *,int,int,const char *);
int wifi_mgr_storage_save_ap(wifi_mgr_sta_config_t *);
int wifi_mgr_sta_connect(wifi_mgr_sta_config_t *,bool);
int wifi_mgr_sta_get_connected_info(wifi_mgr_sta_config_t *);
int wifi_mgr_auto_connect_start(wifi_mgr_autoconn_config_t *);
int wifi_mgr_auto_connect_stop(void);
int wifi_mgr_sta_disconnect(bool);
int wifi_mgr_scan_ap(wifi_mgr_scan_info_t *,int,bool);
int wifi_mgr_sta_enable(void);
int wifi_mgr_sta_add_connection_cb(void (*)(wifi_mgr_connection_info_t *,void *),void *);
void wifi_mgr_init(wifi_mgr_ops_t *);
wifi_mgr_ops_t *wifi_mgr_ops_get(void);
net_if_t *net_if_get(int);
void net_if_up(net_if_t *);
void net_if_down(net_if_t *);
void ls_dhcpc_start(int);
void ls_dhcpc_stop(int);
int app_netdev_register(const char *,int);
int wifi_ps_mode_set(int);
int wifi_get_link_status(struct wifi_link_status *);
mac_manager_t *mac_manager_init(const test_mac_ops_t *,void *,const mac_manager_config_t *);
int mac_manager_get(mac_manager_t *,uint8_t *,int);
test_mac_ops_t *mac_manager_ops_get(void);
void net_dhcp_register_status_callback(void (*)(int,bool,uint32_t,uint32_t,uint32_t,void *),void *);
void lisa_wifi_init(lisa_wifi_ops_t *);
