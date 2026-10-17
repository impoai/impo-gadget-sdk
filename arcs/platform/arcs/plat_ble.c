/*
 * BLE peripheral with one custom 128-bit service: an RX characteristic
 * (write / write without response) and a TX characteristic (read + notify).
 * Runs on the CP core's local ListenAI stack, next to Wi-Fi.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "ble_gap.h"
#include "ble_gatt.h"
#include "ble_prf.h"
#include "ble_adv_data.h"
#include "lisa_bluetooth.h"
#include "lisa_ble_api.h"

#include "platform_arcs.h"

#define TAG "plat_ble"
#include "lisa_log.h"

#define PREF_MTU        247
#define RX_MAX_LEN      512
#define NOTIFY_WAIT_MS  1000
#define UUID_TYPE_128   2           /* gatt_uuid_type: 0=16, 1=32, 2=128 bit */
#define SVC_UUID128     (UUID_TYPE_128 << 5)
#define ATT_UUID128     (UUID_TYPE_128 << 14)
#define NO_CONN         0xFF

/* 16-bit SIG UUID placed in the first two bytes of the 128-bit field. */
#define UUID16(u) { (uint8_t)((u) & 0xFF), (uint8_t)((u) >> 8) }

enum {
    IDX_SVC,
    IDX_RX_CHAR,
    IDX_RX_VAL,
    IDX_TX_CHAR,
    IDX_TX_VAL,
    IDX_TX_CCCD,
    IDX_NB,
};

static struct {
    plat_ble_cbs_t cbs;
    uint8_t svc_uuid[16];
    ble_gatt_att_desc_t db[IDX_NB];
    uint8_t user_lid;
    uint16_t start_hdl;
    volatile uint8_t conidx;
    volatile uint16_t cccd;
    SemaphoreHandle_t sent;
    SemaphoreHandle_t tx_lock;
    bool advertising_wanted;
    bool started;
    uint8_t setup_flag;
    char name[32];
    uint8_t adv[31];
    uint8_t adv_len;
    uint8_t rsp[31];
    uint8_t rsp_len;
} g = { .user_lid = BLE_GATT_INVALID_USER_LID, .conidx = NO_CONN };

/* "7fdd3d1c-38ea-..." -> 16 bytes, LSB first as the stack expects. */
static int parse_uuid128(const char *s, uint8_t out[16])
{
    uint8_t be[16];
    int n = 0;
    for (const char *p = s; *p && n < 16; p++) {
        if (*p == '-') {
            continue;
        }
        unsigned v;
        if (sscanf(p, "%2x", &v) != 1) {
            return -1;
        }
        be[n++] = (uint8_t)v;
        p++;
    }
    if (n != 16) {
        return -1;
    }
    for (int i = 0; i < 16; i++) {
        out[i] = be[15 - i];
    }
    return 0;
}

static void build_adv(void)
{
    uint8_t *a = g.adv, n = 0;
    a[n++] = 2;
    a[n++] = GAP_AD_TYPE_FLAGS;
    a[n++] = GAP_AD_TYPE_FLAGS_GENERAL | GAP_AD_TYPE_FLAGS_BREDR_NOT_SUPPORTED;
    a[n++] = 17;
    a[n++] = GAP_AD_TYPE_128BIT_COMPLETE;
    memcpy(a + n, g.svc_uuid, 16);
    n += 16;
    a[n++] = 4;
    a[n++] = 0xFF; /* manufacturer specific data */
    a[n++] = 0xFF;
    a[n++] = 0xFF;
    a[n++] = g.setup_flag;
    g.adv_len = n;

    size_t name_len = strlen(g.name);
    if (name_len > 29) {
        name_len = 29;
    }
    g.rsp[0] = (uint8_t)(name_len + 1);
    g.rsp[1] = GAP_AD_TYPE_LOCAL_NAME_COMPLETE;
    memcpy(g.rsp + 2, g.name, name_len);
    g.rsp_len = (uint8_t)(name_len + 2);
}

/* Weak hooks read by the stack on every advertising start. */
const uint8_t *lisa_bt_get_adv_data(uint8_t *len)
{
    *len = g.adv_len;
    return g.adv;
}

const uint8_t *lisa_bt_get_scan_rsp_data(uint8_t *len)
{
    *len = g.rsp_len;
    return g.rsp;
}

/* ---- GATT server callbacks (BT task) ---- */

static void cb_event_sent(uint8_t conidx, uint8_t user_lid, uint16_t dummy, uint16_t status)
{
    (void)conidx; (void)user_lid; (void)dummy; (void)status;
    xSemaphoreGive(g.sent);
}

static void cb_att_event_get(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t dummy, uint16_t hdl,
                             uint16_t max_length)
{
    (void)conidx; (void)user_lid; (void)token; (void)dummy; (void)hdl; (void)max_length;
}

static void cb_att_info_get(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl)
{
    ble_gatt_srv_att_info_get_cfm(conidx, user_lid, token, BLE_GAP_ERR_NO_ERROR, RX_MAX_LEN);
    (void)hdl;
}

static void cb_att_val_set(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl, uint16_t offset,
                           void *p_data)
{
    uint16_t status = BLE_GAP_ERR_NO_ERROR;
    uint16_t len = ble_co_buf_data_len(p_data);
    uint8_t *buf = ble_co_buf_data(p_data);
    switch (hdl - g.start_hdl) {
    case IDX_TX_CCCD:
        if (len >= 2) {
            g.cccd = ble_co_read16p(buf);
            LISA_LOGI(TAG, "notifications %s", g.cccd == BLE_PRF_CLI_START_NTF ? "on" : "off");
        } else {
            status = BLE_PRF_ERR_INVALID_PARAM;
        }
        break;
    case IDX_RX_VAL:
        if (offset == 0 && g.cbs.on_write != NULL) {
            g.cbs.on_write(buf, len);
        }
        break;
    default:
        status = BLE_PRF_ERR_INVALID_PARAM;
        break;
    }
    ble_gatt_srv_att_val_set_cfm(conidx, user_lid, token, status);
}

static void cb_att_read_get(uint8_t conidx, uint8_t user_lid, uint16_t token, uint16_t hdl, uint16_t offset,
                            uint16_t max_length)
{
    (void)offset; (void)max_length;
    switch (hdl - g.start_hdl) {
    case IDX_TX_CCCD: {
        uint16_t v = g.cccd;
        ble_gatt_srv_att_read_get_cfm(conidx, user_lid, token, BLE_GAP_ERR_NO_ERROR, 2, 2, (uint8_t *)&v);
        break;
    }
    case IDX_TX_VAL:
        ble_gatt_srv_att_read_get_cfm(conidx, user_lid, token, BLE_GAP_ERR_NO_ERROR, 0, 0, NULL);
        break;
    default:
        ble_gatt_srv_att_read_get_cfm(conidx, user_lid, token, BLE_PRF_ERR_INVALID_PARAM, 0, 0, NULL);
        break;
    }
}

static const ble_gatt_srv_cb_t s_srv_cb = {
    .cb_event_sent = cb_event_sent,
    .cb_att_read_get = cb_att_read_get,
    .cb_att_event_get = cb_att_event_get,
    .cb_att_info_get = cb_att_info_get,
    .cb_att_val_set = cb_att_val_set,
};

static void on_enabled(uint16_t status)
{
    uint16_t r = ble_gatt_user_register(PREF_MTU, 0, &s_srv_cb, &g.user_lid);
    if (r == BLE_GAP_ERR_NO_ERROR) {
        r = ble_gatt_db_svc_add(g.user_lid, SVC_UUID128, g.svc_uuid, IDX_NB, NULL, g.db, IDX_NB, &g.start_hdl);
    }
    LISA_LOGI(TAG, "stack up (status %u), service add %u at handle %u", status, r, g.start_hdl);
    g.started = r == BLE_GAP_ERR_NO_ERROR;
    if (g.started && g.advertising_wanted) {
        lisa_ble_adv_start(0, LISA_BLE_ADV_GEN);
    }
}

static void on_connected(uint8_t conidx, uint16_t conhdl, const gap_bdaddr_t *peer)
{
    (void)conhdl; (void)peer;
    g.conidx = conidx;
    g.cccd = 0;
    LISA_LOGI(TAG, "connected (conidx %u)", conidx);
    if (g.cbs.on_connect != NULL) {
        g.cbs.on_connect();
    }
}

static void on_disconnected(uint8_t conidx, uint16_t conhdl, uint16_t reason)
{
    (void)conidx; (void)conhdl;
    g.conidx = NO_CONN;
    g.cccd = 0;
    LISA_LOGI(TAG, "disconnected (reason 0x%04x)", reason);
    if (g.cbs.on_disconnect != NULL) {
        g.cbs.on_disconnect();
    }
    if (g.advertising_wanted) {
        lisa_ble_adv_start(0, LISA_BLE_ADV_GEN);
    }
}

/* ---- public API ---- */

int plat_ble_start(const char *name, const char *svc_uuid, const char *rx_uuid, const char *tx_uuid,
                   const plat_ble_cbs_t *cbs)
{
    uint8_t rx[16], tx[16];
    if (parse_uuid128(svc_uuid, g.svc_uuid) != 0 || parse_uuid128(rx_uuid, rx) != 0 ||
        parse_uuid128(tx_uuid, tx) != 0) {
        return -1;
    }
    g.cbs = *cbs;
    snprintf(g.name, sizeof(g.name), "%s", name);
    g.sent = xSemaphoreCreateBinary();
    g.tx_lock = xSemaphoreCreateMutex();
    g.advertising_wanted = true;

    const ble_gatt_att_desc_t db[IDX_NB] = {
        [IDX_SVC] = { UUID16(BLE_GATT_DECL_PRIMARY_SERVICE), BLE_PROP(RD), 0 },
        [IDX_RX_CHAR] = { UUID16(BLE_GATT_DECL_CHARACTERISTIC), BLE_PROP(RD), 0 },
        [IDX_RX_VAL] = { {0}, BLE_PROP(WR) | BLE_PROP(WC) | ATT_UUID128, RX_MAX_LEN },
        [IDX_TX_CHAR] = { UUID16(BLE_GATT_DECL_CHARACTERISTIC), BLE_PROP(RD), 0 },
        [IDX_TX_VAL] = { {0}, BLE_PROP(RD) | BLE_PROP(N) | ATT_UUID128, RX_MAX_LEN },
        [IDX_TX_CCCD] = { UUID16(BLE_GATT_DESC_CLIENT_CHAR_CFG), BLE_PROP(RD) | BLE_PROP(WR), BLE_OPT(NO_OFFSET) },
    };
    memcpy(g.db, db, sizeof(db));
    memcpy(g.db[IDX_RX_VAL].uuid, rx, 16);
    memcpy(g.db[IDX_TX_VAL].uuid, tx, 16);
    build_adv();

    /* GAP device name: the phone reads it as well as the advertised one. */
    extern ble_gap_cfg_t bt_stack_dev_cfg;
    size_t nl = strlen(g.name);
    if (nl >= sizeof(bt_stack_dev_cfg.name)) {
        nl = sizeof(bt_stack_dev_cfg.name) - 1;
    }
    memcpy(bt_stack_dev_cfg.name, g.name, nl);
    bt_stack_dev_cfg.name[nl] = '\0';
    bt_stack_dev_cfg.name_len = (uint8_t)(nl + 1);

    lisa_ble_register_conn_cb(on_connected);
    lisa_ble_register_disc_cb(on_disconnected);
    int r = lisa_bluetooth_init(on_enabled);
    LISA_LOGI(TAG, "init %d, name %s", r, g.name);
    return r;
}

void plat_ble_set_advertising(bool on, uint8_t setup_flag)
{
    bool changed = setup_flag != g.setup_flag;
    g.setup_flag = setup_flag;
    build_adv();
    g.advertising_wanted = on;
    if (!g.started) {
        return;
    }
    if (!on || changed) {
        lisa_ble_adv_stop(0);
    }
    if (on && g.conidx == NO_CONN) {
        lisa_ble_adv_start(0, LISA_BLE_ADV_GEN);
    }
}

size_t plat_ble_max_notify(void)
{
    uint8_t c = g.conidx;
    if (c == NO_CONN) {
        return 20;
    }
    uint16_t mtu = ble_gap_get_peer_mtu(c);
    if (mtu < 23) {
        mtu = 23;
    }
    return mtu - 3;
}

int plat_ble_notify(const uint8_t *data, size_t len)
{
    uint8_t c = g.conidx;
    if (c == NO_CONN || g.cccd != BLE_PRF_CLI_START_NTF || len == 0 || len > plat_ble_max_notify()) {
        return -1;
    }
    xSemaphoreTake(g.tx_lock, portMAX_DELAY);
    xSemaphoreTake(g.sent, 0);
    uint16_t r = ble_gatt_srv_event_send(c, g.user_lid, 0, BLE_GATT_NOTIFY, g.start_hdl + IDX_TX_VAL, data,
                                         (uint16_t)len);
    bool done = r == BLE_GAP_ERR_NO_ERROR && xSemaphoreTake(g.sent, pdMS_TO_TICKS(NOTIFY_WAIT_MS)) == pdTRUE;
    xSemaphoreGive(g.tx_lock);
    return done ? 0 : -1;
}

void plat_ble_disconnect(void)
{
    uint8_t c = g.conidx;
    if (c != NO_CONN) {
        lisa_ble_disconnect(c, 0x13);  /* remote user terminated */
    }
}
