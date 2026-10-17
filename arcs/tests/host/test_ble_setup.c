/* Host test: ble_setup state machine, playing the iOS app against the community_v5 vector. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/gcm.h"
#include "cJSON.h"

#include "ble_setup.h"
#include "pairing_crypto.h"
#include "vectors_community_v5.h"

static int failures;
#define CHECK(cond, what)                                              \
    do {                                                               \
        if (cond) printf("ok   %s\n", what);                           \
        else { printf("FAIL %s\n", what); failures++; }                \
    } while (0)

int pairing_rng(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    FILE *f = fopen("/dev/urandom", "rb");
    size_t n = f ? fread(out, 1, len, f) : 0;
    if (f) fclose(f);
    return n == len ? 0 : -1;
}

/* ---- fake transport: collect notifications into whole messages ---- */
static char inbox[16][4096];
static int inbox_n;
static char partial[8192];
static size_t partial_len;
static size_t mtu_payload = 160;
static int disconnects, completes;
static char saved_access[600], saved_ssid[64];

static int fake_notify(const uint8_t *d, size_t n)
{
    if (n > mtu_payload) {
        printf("FAIL packet %zu exceeds %zu\n", n, mtu_payload);
        failures++;
    }
    if (n >= 3 && d[0] == 0xFE) {
        if (d[1] == 0) partial_len = 0;
        memcpy(partial + partial_len, d + 3, n - 3);
        partial_len += n - 3;
        if (d[1] + 1 == d[2]) {
            partial[partial_len] = 0;
            snprintf(inbox[inbox_n++ % 16], 4096, "%s", partial);
        }
    } else {
        snprintf(inbox[inbox_n++ % 16], 4096, "%.*s", (int)n, (const char *)d);
    }
    return 0;
}
static size_t fake_max_packet(void) { return mtu_payload; }
static void fake_disconnect(void) { disconnects++; }
static void fake_sleep(uint32_t ms) { (void)ms; }
static int fake_scan(ble_setup_ap_t *aps, int max)
{
    (void)max;
    snprintf(aps[0].ssid, sizeof(aps[0].ssid), "TestNet");
    aps[0].rssi = -40;
    aps[0].secure = true;
    return 1;
}
static int fake_connect(const char *ssid, const char *pwd, uint32_t t)
{
    (void)t;
    snprintf(saved_ssid, sizeof(saved_ssid), "%s", ssid);
    return strcmp(pwd, "right") == 0 ? 0 : -1;
}
static int fake_save(const char *a, const char *r, const char *api, const char *nh)
{
    (void)r; (void)api; (void)nh;
    snprintf(saved_access, sizeof(saved_access), "%s", a);
    return 0;
}
static void fake_complete(void) { completes++; }

/* ---- the "app" side ---- */
static pair_session_t app;   /* app view: rx_key = device->mobile, tx via app_seal */
static uint8_t app_tx_key[32];
static uint64_t app_tx_ctr;

/* App writes a message chunked the way GadgetPairing.swift does. */
static void app_write(const char *msg)
{
    size_t len = strlen(msg), per = 157;
    size_t total = (len + per - 1) / per;
    char *out = NULL;
    for (size_t i = 0; i < total; i++) {
        uint8_t pkt[160];
        size_t n = (i + 1 < total) ? per : len - i * per;
        pkt[0] = 0xFE; pkt[1] = (uint8_t)i; pkt[2] = (uint8_t)total;
        memcpy(pkt + 3, msg + i * per, n);
        size_t ml = 0;
        out = ble_setup_reassemble(pkt, n + 3, &ml);
        if (out) { ble_setup_handle(out, ml); free(out); }
    }
}

static void app_send_encrypted(const char *pt)
{
    uint8_t nonce[12] = {0}, tag[16], ct[2048];
    for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(app_tx_ctr >> (8 * (7 - i)));
    char aad[96];
    int al = snprintf(aad, sizeof(aad), "hatch-link ble setup v1|%s|m2d|%llu", app.session_id,
                      (unsigned long long)app_tx_ctr);
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, app_tx_key, 256);
    mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, strlen(pt), nonce, 12, (uint8_t *)aad, al,
                              (const uint8_t *)pt, ct, 16, tag);
    mbedtls_gcm_free(&g);
    char ctb[3000], tagb[32], msg[3300];
    b64url_encode(ct, strlen(pt), ctb, sizeof(ctb));
    b64url_encode(tag, 16, tagb, sizeof(tagb));
    snprintf(msg, sizeof(msg),
             "{\"action\":\"pairing_encrypted\",\"session_id\":\"%s\",\"counter\":\"%llu\",\"ciphertext\":\"%s\",\"tag\":\"%s\"}",
             app.session_id, (unsigned long long)app_tx_ctr, ctb, tagb);
    app_tx_ctr++;
    app_write(msg);
}

/* Decrypt the last device message (d2m) with the app's keys; returns the "status" or "type". */
static const char *app_read_last(char *buf, size_t cap)
{
    cJSON *env = cJSON_Parse(inbox[(inbox_n - 1) % 16]);
    const cJSON *ct = cJSON_GetObjectItem(env, "ciphertext"), *tag = cJSON_GetObjectItem(env, "tag");
    const cJSON *ctr = cJSON_GetObjectItem(env, "counter");
    buf[0] = 0;
    if (!cJSON_IsString(ct) || !cJSON_IsString(tag) || !cJSON_IsString(ctr)) { cJSON_Delete(env); return "?"; }
    uint8_t c[3000], t[18], nonce[12] = {1, 0, 0, 0};
    unsigned long long counter = strtoull(ctr->valuestring, NULL, 10);
    for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(counter >> (8 * (7 - i)));
    int n = b64url_decode(ct->valuestring, c, sizeof(c));
    b64url_decode(tag->valuestring, t, sizeof(t));
    char aad[96];
    int al = snprintf(aad, sizeof(aad), "hatch-link ble setup v1|%s|d2m|%llu", app.session_id, counter);
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, app.rx_key, 256);
    int rc = mbedtls_gcm_auth_decrypt(&g, n, nonce, 12, (uint8_t *)aad, al, t, 16, c, (uint8_t *)buf);
    mbedtls_gcm_free(&g);
    cJSON_Delete(env);
    if (rc != 0) return "<bad tag>";
    buf[n] = 0;
    cJSON *in = cJSON_Parse(buf);
    static char out[64];
    const cJSON *s = cJSON_GetObjectItem(in, "status");
    if (!s) s = cJSON_GetObjectItem(in, "type");
    snprintf(out, sizeof(out), "%s", cJSON_IsString(s) ? s->valuestring : "?");
    cJSON_Delete(in);
    return out;
}

static int run(const char *policy, bool vector_keys)
{
    static ble_setup_ops_t ops;
    ops = (ble_setup_ops_t){
        .node_id = V_NODE_ID, .device_id = V_DEVICE_ID, .mac = V_MAC, .firmware_version = V_FIRMWARE_VERSION,
        .policy = policy, .notify = fake_notify, .max_packet = fake_max_packet, .disconnect = fake_disconnect,
        .sleep_ms = fake_sleep, .wifi_scan = fake_scan, .wifi_connect = fake_connect, .save_pairing = fake_save,
        .on_setup_complete = fake_complete,
    };
    ble_setup_init(&ops);
    inbox_n = 0;
    app_tx_ctr = 0;
    ble_setup_rx_reset();
    ble_setup_on_connect();

    app_write("{\"action\":\"get_device_info\"}");
    cJSON *di = cJSON_Parse(inbox[0]);
    CHECK(di && strcmp(cJSON_GetObjectItem(di, "type")->valuestring, "device_info") == 0 &&
              cJSON_GetObjectItem(di, "pairing_protocol")->valueint == 5 &&
              strcmp(cJSON_GetObjectItem(di, "model")->valuestring, "hatch_link") == 0,
          "device_info");
    cJSON_Delete(di);

    uint8_t mob_priv[32], mob_pub[65], mob_nonce[16];
    char mob_pub_b64[96], mob_nonce_b64[32];
    if (vector_keys) {
        uint8_t dpriv[32], dnonce[16];
        for (int i = 0; i < 32; i++) sscanf(V_DEVICE_PRIVATE_SCALAR_HEX + 2 * i, "%2hhx", &dpriv[i]);
        b64url_decode(V_DEVICE_NONCE, dnonce, 16);
        ble_setup_set_test_keys(dpriv, dnonce);
        for (int i = 0; i < 32; i++) sscanf(V_MOBILE_PRIVATE_SCALAR_HEX + 2 * i, "%2hhx", &mob_priv[i]);
        pair_pub_from_priv(mob_priv, mob_pub);
        b64url_decode(V_MOBILE_NONCE, mob_nonce, 16);
    } else {
        pair_keygen(mob_priv, mob_pub);
        pairing_rng(NULL, mob_nonce, 16);
    }
    b64url_encode(mob_pub, 65, mob_pub_b64, sizeof(mob_pub_b64));
    b64url_encode(mob_nonce, 16, mob_nonce_b64, sizeof(mob_nonce_b64));
    char hello[512];
    snprintf(hello, sizeof(hello),
             "{\"action\":\"pairing_client_hello\",\"mobile_nonce\":\"%s\",\"mobile_pub\":\"%s\",\"pairing_auth\":\"none\","
             "\"pairing_policy\":\"%s\",\"version\":5}",
             mob_nonce_b64, mob_pub_b64, policy);
    app_write(hello);

    cJSON *pr = cJSON_Parse(inbox[(inbox_n - 1) % 16]);
    const cJSON *type = cJSON_GetObjectItem(pr, "type");
    CHECK(cJSON_IsString(type) && strcmp(type->valuestring, "pairing_ready") == 0, "pairing_ready");
    if (!cJSON_IsString(type)) { cJSON_Delete(pr); return -1; }

    /* App side: rebuild the transcript from pairing_ready, as GadgetPairing.swift does. */
    const char *dpub = cJSON_GetObjectItem(pr, "device_pub")->valuestring;
    const char *dnonce = cJSON_GetObjectItem(pr, "device_nonce")->valuestring;
    char transcript[1024];
    snprintf(transcript, sizeof(transcript),
             "hatch-link-pairing-v5\nversion=5\ninitiator_role=mobile\nresponder_role=link\ndevice_id=%s\nnode_id=%s\n"
             "mac=%s\nmodel=hatch_link\nfirmware_version=%s\nselected_cipher_suite=p256-hkdf-sha256-aes-gcm-v1\n"
             "pairing_auth=none\npairing_auth_epoch=0\npairing_policy=%s\nconfirm_timeout_seconds=%d\n"
             "mobile_pub=%s\ndevice_pub=%s\nmobile_nonce=%s\ndevice_nonce=%s",
             cJSON_GetObjectItem(pr, "device_id")->valuestring, cJSON_GetObjectItem(pr, "node_id")->valuestring,
             cJSON_GetObjectItem(pr, "mac")->valuestring, cJSON_GetObjectItem(pr, "firmware_version")->valuestring,
             policy, strcmp(policy, "confirm_press") == 0 ? 60 : 0, mob_pub_b64, dpub, mob_nonce_b64, dnonce);
    uint8_t th[32], dpub_raw[65], dn_raw[16], ecdh[32];
    char th_b64[48];
    pair_sha256(transcript, strlen(transcript), th);
    b64url_encode(th, 32, th_b64, sizeof(th_b64));
    CHECK(strcmp(th_b64, cJSON_GetObjectItem(pr, "transcript_hash")->valuestring) == 0, "transcript_hash agrees");
    if (vector_keys) {
        CHECK(strcmp(transcript, V_TRANSCRIPT) == 0, "transcript equals SDK vector");
        CHECK(strcmp(th_b64, V_TRANSCRIPT_HASH) == 0, "transcript_hash equals SDK vector");
    }
    b64url_decode(dpub, dpub_raw, 65);
    b64url_decode(dnonce, dn_raw, 16);
    pair_ecdh(mob_priv, dpub_raw, ecdh);
    pair_session_t dev_view;
    pair_derive(ecdh, mob_nonce, dn_raw, th, &dev_view);
    memset(&app, 0, sizeof(app));
    memcpy(app.rx_key, dev_view.tx_key, 32);
    memcpy(app_tx_key, dev_view.rx_key, 32);
    snprintf(app.session_id, sizeof(app.session_id), "%s", dev_view.session_id);
    CHECK(strcmp(app.session_id, cJSON_GetObjectItem(pr, "session_id")->valuestring) == 0, "session_id agrees");
    cJSON_Delete(pr);

    char buf[4096];
    if (vector_keys) {
        char m[512];
        snprintf(m, sizeof(m),
                 "{\"action\":\"pairing_encrypted\",\"session_id\":\"%s\",\"counter\":\"0\",\"ciphertext\":\"%s\",\"tag\":\"%s\"}",
                 V_SESSION_ID, V_CLIENT_FINISHED_CIPHERTEXT, V_CLIENT_FINISHED_TAG);
        app_write(m);
        app_tx_ctr = 1;
    } else {
        app_send_encrypted("{\"action\":\"pairing_client_finished\"}");
    }
    if (strcmp(policy, "confirm_press") == 0) {
        CHECK(strcmp(app_read_last(buf, sizeof(buf)), "confirm_required") == 0, "confirm_required");
        ble_setup_confirm_press();
    }
    CHECK(strcmp(app_read_last(buf, sizeof(buf)), "pairing_confirmed") == 0, "pairing_confirmed");

    app_send_encrypted("{\"action\":\"wifi_scan\"}");
    CHECK(strcmp(app_read_last(buf, sizeof(buf)), "wifi_scan_result") == 0 && strstr(buf, "\"ssid\":\"TestNet\"") &&
              strstr(buf, "\"secure\":true"),
          "wifi_scan_result");

    int before = inbox_n;
    app_send_encrypted("{\"access_token\":\"AAA\",\"action\":\"provision_v2\",\"api_url_v2\":\"https://gadgets.impo.ai\","
                       "\"noise_host\":\"gadgets.impo.ai\",\"password\":\"wrong\",\"refresh_token\":\"RRR\",\"ssid\":\"TestNet\","
                       "\"token_type\":\"device\",\"username\":\"\"}");
    CHECK(inbox_n - before == 2 && strcmp(app_read_last(buf, sizeof(buf)), "wifi_failed") == 0, "wifi_failed on bad password");

    before = inbox_n;
    app_send_encrypted("{\"access_token\":\"AAA\",\"action\":\"provision_v2\",\"api_url_v2\":\"https://gadgets.impo.ai\","
                       "\"noise_host\":\"gadgets.impo.ai\",\"password\":\"right\",\"refresh_token\":\"RRR\",\"ssid\":\"TestNet\","
                       "\"token_type\":\"device\",\"username\":\"\"}");
    CHECK(inbox_n - before == 3 && strcmp(app_read_last(buf, sizeof(buf)), "auth_ok") == 0, "auth_ok after retry");
    CHECK(strcmp(saved_access, "AAA") == 0 && strcmp(saved_ssid, "TestNet") == 0 && completes == 1, "pairing saved");

    app_write(hello);
    CHECK(strncmp(inbox[(inbox_n - 1) % 16], "error_pairing_unavailable", mtu_payload) == 0, "hello refused once set up");
    ble_setup_on_disconnect();
    return 0;
}

int main(void)
{
    printf("-- confirm_press with the SDK vector keys\n");
    run("confirm_press", true);
    completes = 0;
    printf("-- confirm_app with random keys, MTU 23\n");
    mtu_payload = 20;
    run("confirm_app", false);

    /* Tampered record must end the session. */
    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
