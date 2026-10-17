/* Impo BLE setup protocol v5, device side. See ble_setup.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "ble_setup.h"
#include "pairing_crypto.h"

#ifdef BLE_SETUP_HOST_TEST
#define LOGI(...) (printf("I ble_setup: " __VA_ARGS__), printf("\n"))
#define LOGW(...) (printf("W ble_setup: " __VA_ARGS__), printf("\n"))
#else
#define TAG "ble_setup"
#include "lisa_log.h"
#undef LOGI
#undef LOGW
#define LOGI(...) LISA_LOGI(TAG, __VA_ARGS__)
#define LOGW(...) LISA_LOGW(TAG, __VA_ARGS__)
#endif

#define PROTOCOL_VERSION 5
#define MODEL            "hatch_link"
#define AUTH_NONE        "none"
#define SUITE            "p256-hkdf-sha256-aes-gcm-v1"
#define CHUNK_MAGIC      0xFE
#define MAX_CHUNKS       255
#define NOTIFY_PACING_MS 50
#define WIFI_JOIN_MS     60000

typedef enum {
    ST_IDLE,        /* no session */
    ST_READY,       /* pairing_ready sent, waiting for client_finished */
    ST_WAIT_PRESS,  /* confirm_press: waiting for the button */
    ST_CONFIRMED,   /* encrypted, confirmed: wifi_scan / provision_v2 allowed */
    ST_DONE,
} state_t;

static struct {
    const ble_setup_ops_t *ops;
    state_t state;
    pair_session_t session;
    uint8_t priv[PAIR_PRIV_LEN];
    /* reassembly */
    uint8_t *rx;
    size_t rx_len;
    int rx_total;
    int rx_next;
    /* test hooks */
    bool test_keys;
    uint8_t test_priv[PAIR_PRIV_LEN];
    uint8_t test_nonce[PAIR_NONCE_LEN];
} g;

static void reset_session(void)
{
    pair_wipe(&g.session, sizeof(g.session));
    pair_wipe(g.priv, sizeof(g.priv));
    if (g.state != ST_DONE) {
        g.state = ST_IDLE;
    }
}

void ble_setup_init(const ble_setup_ops_t *ops)
{
    g.ops = ops;
    g.state = ST_IDLE;
}

void ble_setup_set_test_keys(const uint8_t priv[32], const uint8_t nonce[16])
{
    memcpy(g.test_priv, priv, 32);
    memcpy(g.test_nonce, nonce, 16);
    g.test_keys = true;
}

void ble_setup_rx_reset(void)
{
    g.rx_len = 0;
    g.rx_total = 0;
    g.rx_next = 0;
}

void ble_setup_on_connect(void)
{
    reset_session();
}

void ble_setup_on_disconnect(void)
{
    reset_session();
}

void ble_setup_set_paired(bool paired)
{
    reset_session();
    g.state = paired ? ST_DONE : ST_IDLE;
}

/* ---- framing ---- */

char *ble_setup_reassemble(const uint8_t *data, size_t len, size_t *msg_len)
{
    if (len < 3 || data[0] != CHUNK_MAGIC) {  /* unchunked message */
        char *m = malloc(len + 1);
        if (m != NULL) {
            memcpy(m, data, len);
            m[len] = '\0';
            *msg_len = len;
        }
        return m;
    }
    int index = data[1], total = data[2];
    if (total == 0) {
        g.rx_len = 0;
        g.rx_total = 0;
        return NULL;
    }
    if (index == 0 || total != g.rx_total) {
        g.rx_len = 0;
        g.rx_total = total;
        g.rx_next = 0;
    }
    if (index != g.rx_next || g.rx_len + len - 3 > BLE_SETUP_MAX_MESSAGE) {
        g.rx_len = 0;
        g.rx_total = 0;
        return NULL;
    }
    if (g.rx == NULL && (g.rx = malloc(BLE_SETUP_MAX_MESSAGE + 1)) == NULL) {
        return NULL;
    }
    memcpy(g.rx + g.rx_len, data + 3, len - 3);
    g.rx_len += len - 3;
    g.rx_next++;
    if (g.rx_next < total) {
        return NULL;
    }
    char *m = malloc(g.rx_len + 1);
    if (m != NULL) {
        memcpy(m, g.rx, g.rx_len);
        m[g.rx_len] = '\0';
        *msg_len = g.rx_len;
    }
    g.rx_len = 0;
    g.rx_total = 0;
    return m;
}

static void send_framed(const char *msg)
{
    size_t len = strlen(msg);
    size_t per = g.ops->max_packet();
    if (per <= 3) {
        return;
    }
    per -= 3;
    size_t total = (len + per - 1) / per;
    if (total == 0 || total > MAX_CHUNKS) {
        LOGW("message of %u bytes cannot be framed", (unsigned)len);
        return;
    }
    uint8_t pkt[3 + 512];
    for (size_t i = 0; i < total; i++) {
        size_t n = (i + 1 < total) ? per : len - i * per;
        pkt[0] = CHUNK_MAGIC;
        pkt[1] = (uint8_t)i;
        pkt[2] = (uint8_t)total;
        memcpy(pkt + 3, msg + i * per, n);
        g.ops->notify(pkt, n + 3);
        g.ops->sleep_ms(NOTIFY_PACING_MS);
    }
}

/* Plaintext status string, only allowed before pairing_ready. */
static void send_plain_error(const char *status)
{
    /* Bare string, never framed (ble_server.c). The app ignores these, so a tiny MTU may cut one short. */
    size_t len = strlen(status), max = g.ops->max_packet();
    g.ops->notify((const uint8_t *)status, len < max ? len : max);
}

static void send_json(cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    if (s != NULL) {
        send_framed(s);
        cJSON_free(s);
    }
}

static void send_encrypted_obj(cJSON *obj)
{
    char *pt = cJSON_PrintUnformatted(obj);
    if (pt == NULL) {
        return;
    }
    size_t len = strlen(pt);
    size_t cap = len * 4 / 3 + 8;
    char *ct = malloc(cap);
    char tag[32];
    uint64_t counter = 0;
    if (ct != NULL && pair_seal(&g.session, (const uint8_t *)pt, len, &counter, ct, cap, tag) == 0) {
        char cs[24];
        snprintf(cs, sizeof(cs), "%llu", (unsigned long long)counter);
        cJSON *env = cJSON_CreateObject();
        cJSON_AddStringToObject(env, "type", "pairing_encrypted");
        cJSON_AddStringToObject(env, "session_id", g.session.session_id);
        cJSON_AddStringToObject(env, "counter", cs);
        cJSON_AddStringToObject(env, "ciphertext", ct);
        cJSON_AddStringToObject(env, "tag", tag);
        send_json(env);
        cJSON_Delete(env);
    }
    pair_wipe(pt, len);
    cJSON_free(pt);
    free(ct);
}

static void send_status(const char *status)
{
    LOGI("status %s", status);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "status");
    cJSON_AddStringToObject(o, "status", status);
    send_encrypted_obj(o);
    cJSON_Delete(o);
}

static void fail_session(void)
{
    send_plain_error("error_pairing_decrypt");
    reset_session();
    g.ops->sleep_ms(300);
    g.ops->disconnect();
}

/* ---- handlers ---- */

static bool is_press(void)
{
    return strcmp(g.ops->policy, "confirm_press") == 0;
}

static void handle_device_info(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "device_info");
    cJSON_AddStringToObject(o, "node_id", g.ops->node_id);
    cJSON_AddStringToObject(o, "version", g.ops->firmware_version);
    cJSON_AddStringToObject(o, "device_id", g.ops->device_id);
    cJSON_AddStringToObject(o, "mac", g.ops->mac);
    cJSON_AddStringToObject(o, "model", MODEL);
    cJSON_AddNumberToObject(o, "pairing_protocol", PROTOCOL_VERSION);
    cJSON_AddStringToObject(o, "pairing_auth", AUTH_NONE);
    cJSON_AddNumberToObject(o, "pairing_auth_epoch", 0);
    cJSON_AddStringToObject(o, "pairing_policy", g.ops->policy);
    cJSON_AddStringToObject(o, "build_sha", "");
    cJSON_AddBoolToObject(o, "network_ready", true);
    send_json(o);
    cJSON_Delete(o);
}

static const char *str_item(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void handle_hello(const cJSON *m)
{
    if (g.state == ST_DONE) {
        send_plain_error("error_pairing_unavailable");
        return;
    }
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(m, "version");
    const char *auth = str_item(m, "pairing_auth");
    const char *policy = str_item(m, "pairing_policy");
    const char *pub_b64 = str_item(m, "mobile_pub");
    const char *nonce_b64 = str_item(m, "mobile_nonce");
    uint8_t mobile_pub[PAIR_PUB_LEN + 2], mobile_nonce[PAIR_NONCE_LEN + 2];
    if (!cJSON_IsNumber(ver) || ver->valueint != PROTOCOL_VERSION || auth == NULL || strcmp(auth, AUTH_NONE) != 0 ||
        policy == NULL || strcmp(policy, g.ops->policy) != 0 || pub_b64 == NULL || nonce_b64 == NULL ||
        b64url_decode(pub_b64, mobile_pub, sizeof(mobile_pub)) != PAIR_PUB_LEN || mobile_pub[0] != 0x04 ||
        b64url_decode(nonce_b64, mobile_nonce, sizeof(mobile_nonce)) != PAIR_NONCE_LEN) {
        LOGW("invalid pairing_client_hello");
        send_plain_error("error_pairing_invalid_hello");
        return;
    }

    uint8_t device_pub[PAIR_PUB_LEN], device_nonce[PAIR_NONCE_LEN], ecdh[32], th[PAIR_HASH_LEN];
    int ok;
    if (g.test_keys) {
        memcpy(g.priv, g.test_priv, sizeof(g.priv));
        memcpy(device_nonce, g.test_nonce, sizeof(device_nonce));
        ok = pair_pub_from_priv(g.priv, device_pub) == 0;
    } else {
        ok = pair_keygen(g.priv, device_pub) == 0 && pairing_rng(NULL, device_nonce, sizeof(device_nonce)) == 0;
    }
    if (!ok || pair_ecdh(g.priv, mobile_pub, ecdh) != 0) {
        send_plain_error("error_pairing_invalid_hello");
        reset_session();
        return;
    }

    char device_pub_b64[96], device_nonce_b64[32], th_b64[48];
    b64url_encode(device_pub, sizeof(device_pub), device_pub_b64, sizeof(device_pub_b64));
    b64url_encode(device_nonce, sizeof(device_nonce), device_nonce_b64, sizeof(device_nonce_b64));

    /* The app rebuilds this from pairing_ready, so every value must be the string we send. */
    char *transcript = malloc(1024);
    if (transcript == NULL) {
        reset_session();
        return;
    }
    int tl = snprintf(transcript, 1024,
                      "hatch-link-pairing-v%d\nversion=%d\ninitiator_role=mobile\nresponder_role=link\n"
                      "device_id=%s\nnode_id=%s\nmac=%s\nmodel=%s\nfirmware_version=%s\n"
                      "selected_cipher_suite=%s\npairing_auth=%s\npairing_auth_epoch=0\npairing_policy=%s\n"
                      "confirm_timeout_seconds=%d\nmobile_pub=%s\ndevice_pub=%s\nmobile_nonce=%s\ndevice_nonce=%s",
                      PROTOCOL_VERSION, PROTOCOL_VERSION, g.ops->device_id, g.ops->node_id, g.ops->mac, MODEL,
                      g.ops->firmware_version, SUITE, AUTH_NONE, g.ops->policy, is_press() ? 60 : 0, pub_b64,
                      device_pub_b64, nonce_b64, device_nonce_b64);
    ok = tl > 0 && tl < 1024 && pair_sha256(transcript, (size_t)tl, th) == 0 &&
         pair_derive(ecdh, mobile_nonce, device_nonce, th, &g.session) == 0;
    free(transcript);
    pair_wipe(ecdh, sizeof(ecdh));
    if (!ok) {
        send_plain_error("error_pairing_invalid_hello");
        reset_session();
        return;
    }
    b64url_encode(th, sizeof(th), th_b64, sizeof(th_b64));

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "pairing_ready");
    cJSON_AddNumberToObject(o, "version", PROTOCOL_VERSION);
    cJSON_AddStringToObject(o, "device_id", g.ops->device_id);
    cJSON_AddStringToObject(o, "node_id", g.ops->node_id);
    cJSON_AddStringToObject(o, "mac", g.ops->mac);
    cJSON_AddStringToObject(o, "model", MODEL);
    cJSON_AddStringToObject(o, "firmware_version", g.ops->firmware_version);
    cJSON_AddStringToObject(o, "pairing_auth", AUTH_NONE);
    cJSON_AddNumberToObject(o, "pairing_auth_epoch", 0);
    cJSON_AddStringToObject(o, "pairing_policy", g.ops->policy);
    cJSON_AddStringToObject(o, "device_pub", device_pub_b64);
    cJSON_AddStringToObject(o, "device_nonce", device_nonce_b64);
    cJSON_AddStringToObject(o, "transcript_hash", th_b64);
    cJSON_AddStringToObject(o, "session_id", g.session.session_id);
    send_json(o);
    cJSON_Delete(o);
    g.state = ST_READY;
    LOGI("pairing_ready sent");
}

static void handle_wifi_scan(void)
{
    ble_setup_ap_t aps[16];
    int n = g.ops->wifi_scan(aps, 16);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "wifi_scan_result");
    cJSON *arr = cJSON_AddArrayToObject(o, "networks");
    for (int i = 0; i < n; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(ap, "secure", aps[i].secure);
        cJSON_AddItemToArray(arr, ap);
    }
    send_encrypted_obj(o);
    cJSON_Delete(o);
}

static void handle_provision(const cJSON *m)
{
    const char *ssid = str_item(m, "ssid");
    const char *pwd = str_item(m, "password");
    const char *access = str_item(m, "access_token");
    const char *refresh = str_item(m, "refresh_token");
    const char *type = str_item(m, "token_type");
    const char *api = str_item(m, "api_url_v2");
    const char *noise = str_item(m, "noise_host");
    if (ssid == NULL || pwd == NULL || ssid[0] == '\0' || access == NULL || access[0] == '\0' || refresh == NULL ||
        refresh[0] == '\0' || type == NULL || strcmp(type, "device") != 0) {
        send_status("error_missing_credentials");
        return;
    }
    send_status("wifi_connecting");
    if (g.ops->wifi_connect(ssid, pwd, WIFI_JOIN_MS) != 0) {
        send_status("wifi_failed");  /* stay confirmed: the app may retry */
        return;
    }
    send_status("wifi_connected");
    if (g.ops->save_pairing(access, refresh, api ? api : "", noise ? noise : "") != 0) {
        send_status("error_storage");
        g.ops->sleep_ms(300);
        g.ops->disconnect();
        return;
    }
    send_status("auth_ok");
    g.state = ST_DONE;
    pair_wipe(&g.session, sizeof(g.session));
    g.ops->sleep_ms(1500);
    g.ops->on_setup_complete();
}

static void handle_encrypted(const cJSON *m)
{
    if (g.state == ST_IDLE || g.state == ST_DONE) {
        send_plain_error("error_pairing_decrypt");
        return;
    }
    const char *sid = str_item(m, "session_id");
    const char *ctr = str_item(m, "counter");
    const char *ct = str_item(m, "ciphertext");
    const char *tag = str_item(m, "tag");
    if (sid == NULL || ctr == NULL || ct == NULL || tag == NULL) {
        fail_session();
        return;
    }
    size_t cap = strlen(ct) * 3 / 4 + 2;
    uint8_t *pt = malloc(cap);
    int n = pt ? pair_open(&g.session, sid, ctr, ct, tag, pt, cap) : -1;
    if (n < 0) {
        LOGW("record rejected");
        free(pt);
        fail_session();
        return;
    }
    cJSON *inner = cJSON_ParseWithLength((const char *)pt, (size_t)n);
    pair_wipe(pt, (size_t)n);
    free(pt);
    const char *action = str_item(inner, "action");
    if (action == NULL) {
        send_status("error_invalid_command");
    } else if (g.state == ST_READY) {
        if (strcmp(action, "pairing_client_finished") == 0 && cJSON_GetArraySize(inner) == 1) {
            if (is_press()) {
                g.state = ST_WAIT_PRESS;
                send_status("confirm_required");
            } else {
                g.state = ST_CONFIRMED;
                send_status("pairing_confirmed");
            }
        } else {
            cJSON_Delete(inner);
            fail_session();
            return;
        }
    } else if (g.state == ST_WAIT_PRESS) {
        send_status("error_pairing_confirm_required");
    } else if (strcmp(action, "wifi_scan") == 0) {
        handle_wifi_scan();
    } else if (strcmp(action, "provision_v2") == 0) {
        handle_provision(inner);
    } else {
        send_status("error_unknown_action");
    }
    cJSON_Delete(inner);
}

void ble_setup_confirm_press(void)
{
    if (g.state == ST_WAIT_PRESS) {
        g.state = ST_CONFIRMED;
        send_status("pairing_confirmed");
    }
}

void ble_setup_handle(const char *msg, size_t len)
{
    cJSON *m = cJSON_ParseWithLength(msg, len);
    const char *action = str_item(m, "action");
    if (action == NULL) {
        LOGW("ignoring non-command message (%u bytes)", (unsigned)len);
    } else if (strcmp(action, "get_device_info") == 0) {
        handle_device_info();
    } else if (strcmp(action, "pairing_client_hello") == 0) {
        handle_hello(m);
    } else if (strcmp(action, "pairing_encrypted") == 0) {
        handle_encrypted(m);
    } else if (g.state == ST_IDLE) {
        send_plain_error("error_encryption_required");
    } else {
        LOGW("plaintext %s ignored inside a session", action);
    }
    cJSON_Delete(m);
}
