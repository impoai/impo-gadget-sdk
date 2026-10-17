/*
 * L3 pairing service: ties the BLE setup protocol to the ARCS BLE peripheral,
 * Wi-Fi and the link credentials. Advertises while the device is unpaired, so
 * the Impo iOS app can find it, and stops once provisioning succeeds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "platform_arcs.h"
#include "link_creds.h"
#include "link_identity.h"
#include "ble_setup.h"
#include "pairing_service.h"

#define TAG "pairing"
#include "lisa_log.h"

#define MAX_NOTIFY 160   /* the app reassembles at most 160-byte packets */

typedef enum { EV_CONNECT, EV_DISCONNECT, EV_MESSAGE } ev_type_t;

typedef struct {
    ev_type_t type;
    char *msg;
    size_t len;
} ev_t;

static QueueHandle_t s_q;
static volatile bool s_connected;

int pairing_rng(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    return plat_random(out, len);
}

static void post(ev_type_t type, char *msg, size_t len)
{
    ev_t ev = { type, msg, len };
    if (xQueueSend(s_q, &ev, 0) != pdTRUE) {
        LISA_LOGW(TAG, "event queue full; dropped %d", (int)type);
        free(msg);
    }
}

/* ---- BLE callbacks (BT task) ---- */

static void on_connect(void)
{
    s_connected = true;
    ble_setup_rx_reset();
    post(EV_CONNECT, NULL, 0);
}

static void on_disconnect(void)
{
    s_connected = false;
    ble_setup_rx_reset();
    post(EV_DISCONNECT, NULL, 0);
}

static void on_write(const uint8_t *data, size_t len)
{
    size_t n = 0;
    char *msg = ble_setup_reassemble(data, len, &n);
    if (msg != NULL) {
        post(EV_MESSAGE, msg, n);
    }
}

/* ---- ops for the protocol (worker task) ---- */

static size_t op_max_packet(void)
{
    size_t m = plat_ble_max_notify();
    return m > MAX_NOTIFY ? MAX_NOTIFY : m;
}

static void op_sleep(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static int op_scan(ble_setup_ap_t *aps, int max)
{
    plat_wifi_ap_t found[16];
    int n = plat_wifi_scan(found, max < 16 ? max : 16);
    for (int i = 0; i < n; i++) {
        snprintf(aps[i].ssid, sizeof(aps[i].ssid), "%s", found[i].ssid);
        aps[i].rssi = found[i].rssi;
        aps[i].secure = found[i].secure;
    }
    return n;
}

static int op_save(const char *access, const char *refresh, const char *api_url, const char *noise_host)
{
    if (strlen(access) >= sizeof(((link_creds_t *)0)->access) ||
        strlen(refresh) >= sizeof(((link_creds_t *)0)->refresh)) {
        LISA_LOGE(TAG, "tokens too long");
        return -1;
    }
    if (api_url[0] && strcmp(api_url, CONFIG_GADGET_API_URL) != 0) {
        LISA_LOGW(TAG, "app sent api_url_v2 %s; this build uses %s", api_url, CONFIG_GADGET_API_URL);
    }
    (void)noise_host;
    link_creds_t c = {0};
    strcpy(c.access, access);
    strcpy(c.refresh, refresh);
    int r = link_creds_save(&c);
    pair_wipe(&c, sizeof(c));
    return r;
}

static void op_complete(void)
{
    LISA_LOGI(TAG, "setup complete; stopping advertising");
    plat_ble_disconnect();
    plat_ble_set_advertising(false, 1);
}

static ble_setup_ops_t s_ops;

static void pairing_task(void *arg)
{
    (void)arg;
    link_creds_t c;
    bool paired = link_creds_load(&c) == 0;
    pair_wipe(&c, sizeof(c));
    unsigned gen = link_creds_generation();
    ble_setup_set_paired(paired);
    plat_ble_set_advertising(!paired, paired ? 1 : 0);
    LISA_LOGI(TAG, "%s; advertising as %s", paired ? "paired" : "unpaired", gadget_ble_name());

    for (;;) {
        ev_t ev;
        if (xQueueReceive(s_q, &ev, pdMS_TO_TICKS(500)) == pdTRUE) {
            switch (ev.type) {
            case EV_CONNECT:
                ble_setup_on_connect();
                break;
            case EV_DISCONNECT:
                ble_setup_on_disconnect();
                break;
            case EV_MESSAGE:
                ble_setup_handle(ev.msg, ev.len);
                pair_wipe(ev.msg, ev.len);
                free(ev.msg);
                break;
            }
        }
        /* Unpaired from Impo or by `pair clear`: advertise again. */
        if (link_creds_generation() != gen) {
            gen = link_creds_generation();
            bool now = link_creds_load(&c) == 0;
            pair_wipe(&c, sizeof(c));
            if (now != paired) {
                paired = now;
                ble_setup_set_paired(paired);
                plat_ble_set_advertising(!paired, paired ? 1 : 0);
                LISA_LOGI(TAG, "%s", paired ? "paired; advertising off" : "unpaired; advertising");
            }
        }
    }
}

int pairing_service_start(void)
{
    s_ops = (ble_setup_ops_t){
        .node_id = gadget_node_id(),
        .device_id = gadget_device_id(),
        .mac = gadget_mac_str(),
        .firmware_version = plat_firmware_version(),
        .policy = "confirm_app",
        .notify = plat_ble_notify,
        .max_packet = op_max_packet,
        .disconnect = plat_ble_disconnect,
        .sleep_ms = op_sleep,
        .wifi_scan = op_scan,
        .wifi_connect = plat_wifi_join,
        .save_pairing = op_save,
        .on_setup_complete = op_complete,
    };
    ble_setup_init(&s_ops);
    s_q = xQueueCreate(8, sizeof(ev_t));
    if (s_q == NULL) {
        return -1;
    }
    static const plat_ble_cbs_t cbs = { on_connect, on_disconnect, on_write };
    if (plat_ble_start(gadget_ble_name(), BLE_SETUP_SERVICE_UUID, BLE_SETUP_RX_UUID, BLE_SETUP_TX_UUID, &cbs) != 0) {
        LISA_LOGE(TAG, "BLE start failed");
        return -1;
    }
    /* Crypto (P-256) and Wi-Fi join run here. */
    return xTaskCreate(pairing_task, "pairing", 12 * 1024 / sizeof(StackType_t), NULL, 4, NULL) == pdPASS ? 0 : -1;
}
