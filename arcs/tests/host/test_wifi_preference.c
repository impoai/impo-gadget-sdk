/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include "../../platform/arcs/net_wifi.c"
static char saved_pref[33];
static wifi_mgr_sta_config_t stored,connected;
static unsigned joins,autos,saves,prefs,deleted;
static unsigned route_resets;
void impo_cloud_route_reset(void) { route_resets++; }
static bool wrong_network;
static int join_error; static bool found=true,kv_present=true;
void test_log(const char *t,const char *f,...) {(void)t;(void)f;}
int lisa_kv_get_string(const char *k,char **v) { assert(!strcmp(k,KV_PREFERRED_WIFI));*v=kv_present?strdup(saved_pref):NULL;return kv_present?0:-1; }
int lisa_kv_set_string(const char *k,const char *v) {assert(!strcmp(k,KV_PREFERRED_WIFI));snprintf(saved_pref,sizeof(saved_pref),"%s",v);prefs++;return 0;}
void lisa_kv_free(void *p) {free(p);}
int wifi_mgr_storage_search_ap(wifi_mgr_sta_config_t *c,int n,int mode,const char *name)
{assert(n==1&&mode==SEARCH_BY_SSID);if(!found||strcmp(name,stored.ssid))return 0;*c=stored;return 1;}
int wifi_mgr_storage_save_ap(wifi_mgr_sta_config_t *c) {stored=*c;saves++;return 0;}
int wifi_mgr_sta_connect(wifi_mgr_sta_config_t *c,bool async)
{assert(!async);assert(c->bssid[0]==0&&c->channel==0);joins++;if(!join_error){s_wifi.ip=1;connected=*c;if(wrong_network)strcpy(connected.ssid,"other");}return join_error;}
int wifi_mgr_sta_get_connected_info(wifi_mgr_sta_config_t *c){*c=connected;return 0;}
int wifi_mgr_auto_connect_start(wifi_mgr_autoconn_config_t *c){assert(c->interval_ms==3000);autos++;return 0;}
int wifi_mgr_auto_connect_stop(void){return 0;}
int wifi_mgr_sta_disconnect(bool a){assert(!a);s_wifi.associated=false;s_wifi.ip=0;return 0;}
void vTaskDelay(unsigned ms){(void)ms;}
void vTaskDelete(TaskHandle_t t){assert(!t);deleted++;}
int xTaskCreate(void (*f)(void *),const char *n,unsigned d,void *a,unsigned p,TaskHandle_t *h)
{(void)f;(void)n;(void)d;(void)a;(void)p;(void)h;return pdPASS;}
/* Unused platform entry points: real preference/join paths above remain under test. */
int wifi_mgr_scan_ap(wifi_mgr_scan_info_t *p,int n,bool b){(void)p;(void)n;(void)b;return 0;}
int wifi_mgr_sta_enable(void){return 0;}
int wifi_mgr_sta_add_connection_cb(void (*f)(wifi_mgr_connection_info_t *,void *),void *p){(void)f;(void)p;return 0;}
static wifi_manager_wifi_ops_t wops;static wifi_mgr_ops_t ops={&wops};
void wifi_mgr_init(wifi_mgr_ops_t *o){(void)o;}
wifi_mgr_ops_t *wifi_mgr_ops_get(void){return &ops;}
static net_if_t nif;
net_if_t *net_if_get(int i){(void)i;return &nif;}
void net_if_up(net_if_t *n){(void)n;}void net_if_down(net_if_t *n){(void)n;}
void ls_dhcpc_start(int n){(void)n;}void ls_dhcpc_stop(int n){(void)n;}
int app_netdev_register(const char *n,int p){(void)n;(void)p;return 0;}
int wifi_ps_mode_set(int p){(void)p;return 0;}
int wifi_get_link_status(struct wifi_link_status *p){(void)p;return -1;}
static mac_manager_t mac;static test_mac_ops_t mops;
mac_manager_t *mac_manager_init(const test_mac_ops_t *o,void *p,const mac_manager_config_t *c){(void)o;(void)p;(void)c;return &mac;}
int mac_manager_get(mac_manager_t *m,uint8_t *p,int n){(void)m;(void)p;return n;}
test_mac_ops_t *mac_manager_ops_get(void){return &mops;}
void net_dhcp_register_status_callback(void (*f)(int,bool,uint32_t,uint32_t,uint32_t,void *),void *p){(void)f;(void)p;}
void lisa_wifi_init(lisa_wifi_ops_t *p){(void)p;}
uint64_t plat_uptime_ms(void){return 0;}
static void reset(void)
{
 memset(&s_wifi,0,sizeof(s_wifi));s_wifi.ready=true;
 strcpy(saved_pref,"chosen");stored=(wifi_mgr_sta_config_t){0};strcpy(stored.ssid,"chosen");strcpy(stored.pwd,"test-password");
 strcpy(stored.bssid,"locked-router");stored.channel=11;connected=(wifi_mgr_sta_config_t){0};
 joins=autos=saves=prefs=deleted=0;join_error=0;wrong_network=false;kv_present=found=true;
}
int main(void)
{
 reset();dhcp_status_cb(WIFI_VIF_STA_IDX,true,1,0,0,NULL);assert(route_resets==1);
 dhcp_status_cb(WIFI_VIF_STA_IDX,true,1,0,0,NULL);assert(route_resets==1);
 dhcp_status_cb(WIFI_VIF_STA_IDX,true,2,0,0,NULL);assert(route_resets==2);
 dhcp_status_cb(WIFI_VIF_STA_IDX,false,0,0,0,NULL);
 dhcp_status_cb(WIFI_VIF_STA_IDX,true,2,0,0,NULL);assert(route_resets==3);
 reset();preferred_boot_task(NULL);assert(joins==1&&autos==1&&deleted==1&&s_wifi.ip);
 reset();kv_present=false;preferred_boot_task(NULL);assert(!joins&&autos==1&&deleted==1);
 reset();found=false;preferred_boot_task(NULL);assert(!joins&&autos==1);
 reset();join_error=-1;preferred_boot_task(NULL);assert(joins==1&&autos==1&&!s_wifi.ip);
 reset();assert(plat_wifi_join("new-network","test-password",1000)==0);assert(prefs==1&&saves==1&&!strcmp(saved_pref,"new-network"));
 reset();wrong_network=true;assert(plat_wifi_join("new-network","test-password",1000)==-1);assert(!prefs&&!saves);
 reset();join_error=-1;assert(plat_wifi_join("new-network","test-password",1000)==-1);assert(!prefs&&!saves&&!strcmp(saved_pref,"chosen"));
 reset();s_wifi.ip=1;strcpy(connected.ssid,"already");strcpy(connected.pwd,"test-password");
 assert(plat_wifi_join("already","test-password",1000)==0);assert(!joins&&prefs==1&&!strcmp(saved_pref,"already"));
 reset();char *args[]={"wifi","connect","cli-network","test-password"};wifi_cmd(4,args);assert(prefs==1&&!strcmp(saved_pref,"cli-network"));
 puts("Wi-Fi preference: selected network first, missing/failed fallback, success-only save and CLI join passed");
}
