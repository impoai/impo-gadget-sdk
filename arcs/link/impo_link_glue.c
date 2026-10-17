/*
 * L1 link: the Impo link every gadget runs (esp32/main, built unchanged),
 * run for ARCS behind impo_link.h.
 *
 *   vm_api.c          device token refresh, GET /fetch_vms
 *   noise_control.cpp TLS, WebSocket, Noise, link.register / invoke / result /
 *                     event / heartbeat, request streams, keepalive, reconnect
 *
 * This file is what upstream's esp32/main/app.c does around them (8f07ace):
 * start the session once Wi-Fi and the clock are up, refresh the VM
 * credentials when the session reports them refused, and forget the pairing
 * when Impo removes the gadget. On top of that, ARCS behaviour upstream does
 * not have: the session is dropped as soon as Wi-Fi is lost (a TCP connection
 * cannot survive it, and noticing takes minutes) and when the gateway has
 * been silent for 150 s, and `link` lists the recent session ends.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "cJSON.h"

#include "sysutils.h"

#include "platform_arcs.h"
#include "esp_compat.h"
#include "noise_control.h"
#include "vm_api.h"
#include "vm_connect.h"

#include "gadget_core.h"
#include "impo_link.h"
#include "impo_link_internal.h"
#include "link_creds.h"
#include "link_glue_logic.h"
#include "link_identity.h"

#define TAG "link"
#include "lisa_log.h"

#define KEEPER_STACK_BYTES (16 * 1024)    /* HTTPS: a TLS handshake, vm_api's 2 KB header buffers */
#define KEEPER_TICK_MS     500
/* Longest the keeper may go without a check-in: three HTTPS calls with their retries on a slow network. */
#define KEEPER_WDT_MS      180000
/* The session task checks in every loop turn and while it waits; a TLS handshake and the 60 s
 * auth backoff are its longest quiet spells. */
#define SESSION_WDT_MS     180000
#define NO_IP_MS           3000           /* Wi-Fi without an address this long: the socket is gone */
#define SILENT_MS          150000         /* upstream pings after 60 s idle; nothing at all back by now: dead */
#define RENAME_DELAY_MS    300            /* as upstream ws_control_task: let the command's result go out */

/* In PSRAM (__psram_bss__): internal SRAM is nearly full. */
static __psram_bss__ struct {
    volatile impo_link_state_t state;
    char vm_id[128];
    volatile bool drop, rename, refresh_needed, unpaired;
    const char *volatile ending;     /* why the keeper is ending the session, for the history */
    volatile bool up;
    uint64_t up_at_ms;
    volatile uint32_t sessions, invokes;
    int wd_keeper, wd_session;
    /* VM credential refresh pacing (upstream app.c s_vm_auth_*). */
    uint64_t last_refresh_ms;
    unsigned refusals;
    link_history_t history;
} g;

static const char *state_name(impo_link_state_t s)
{
    switch (s) {
    case IMPO_LINK_IDLE: return "idle";
    case IMPO_LINK_UNPAIRED: return "unpaired";
    case IMPO_LINK_FETCHING: return "fetching";
    case IMPO_LINK_CONNECTING: return "connecting";
    case IMPO_LINK_REGISTERING: return "registering";
    case IMPO_LINK_ONLINE: return "online";
    }
    return "?";
}

static void set_state(impo_link_state_t s)
{
    if (g.state != s) {
        LISA_LOGI(TAG, "state %s -> %s", state_name(g.state), state_name(s));
        g.state = s;
    }
}

/* ---- callbacks from the session task ---- */

/* Adapted from upstream app.c on_ws_control_status. */
static void on_status(const char *status)
{
    if (strcmp(status, "ws_connected") == 0) {
        g.up = true;
        g.up_at_ms = plat_uptime_ms();
        g.sessions++;
        set_state(IMPO_LINK_ONLINE);
    } else if (strcmp(status, "ws_disconnected") == 0) {
        char why[40];
        if (g.ending != NULL) {
            snprintf(why, sizeof(why), "%s", g.ending);
        } else {
            esp_compat_tls_last_event(why, sizeof(why));
        }
        link_history_add(&g.history, (uint32_t)(plat_uptime_ms() / 1000),
                         (uint32_t)((plat_uptime_ms() - g.up_at_ms) / 1000), esp_compat_tls_rx_quiet_ms(),
                         g.invokes, why);
        g.up = false;
        set_state(IMPO_LINK_CONNECTING);
    } else if (strcmp(status, "ws_unpaired") == 0) {
        /* The keeper resets: this runs on the session task, which noise_ctrl_disconnect() waits for. */
        LISA_LOGW(TAG, "Impo removed this device");
        g.unpaired = true;
    } else if (strcmp(status, "ws_auth_failed") == 0 || strcmp(status, "ws_refresh_needed") == 0) {
        LISA_LOGW(TAG, "session needs fresh VM credentials: %s", status);
        g.refresh_needed = true;
    }
}

static cJSON *on_command(const char *command, cJSON *params, const char *request_id,
                         noise_ctrl_session_generation_t session_generation)
{
    (void)request_id;
    (void)session_generation;
    g.invokes++;
    cJSON *r = gadget_run_command(command, params);
    link_result_to_upstream(r);
    return r;
}

/* ---- starting and stopping the session ---- */

static void stop_session(const char *why)
{
    if (!noise_ctrl_is_running()) {
        return;
    }
    LISA_LOGW(TAG, "ending the session: %s", why);
    g.ending = why;
    noise_ctrl_disconnect();
    g.ending = NULL;
    plat_watchdog_pause(g.wd_session, true);
    set_state(IMPO_LINK_IDLE);
}

static const char *wifi_ssid(char *buf, size_t cap)
{
    return plat_wifi_get_ssid(buf, cap) == 0 && buf[0] != '\0' ? buf : NULL;
}

/* Adapted from upstream app.c connect_vm_target. */
static bool connect_vm_target(const vm_info_t *target)
{
    char vm_id[128];
    const char *auth_token = NULL;
    if (!vm_connect_params(target, vm_id, sizeof(vm_id), &auth_token)) {
        LISA_LOGE(TAG, "no usable vm_id/token for VM entry");
        return false;
    }
    char ssid[33];
    set_state(IMPO_LINK_CONNECTING);
    plat_watchdog_pause(g.wd_session, false);
    if (!noise_ctrl_connect(vm_id, auth_token, wifi_ssid(ssid, sizeof(ssid)))) {
        plat_watchdog_pause(g.wd_session, true);
        LISA_LOGE(TAG, "noise control start failed for %s", target->vm_url);
        return false;
    }
    snprintf(g.vm_id, sizeof(g.vm_id), "%s", vm_id);
    LISA_LOGI(TAG, "connecting to VM %s", vm_id);
    return true;
}

/* The same VM with the same token, without fetching the VM list again
 * (upstream app.c impo_keep_vm_session, s_impo_resume_vm). */
static bool resume_session(void)
{
    char ssid[33];
    set_state(IMPO_LINK_CONNECTING);
    plat_watchdog_pause(g.wd_session, false);
    if (!noise_ctrl_reconnect(wifi_ssid(ssid, sizeof(ssid)))) {
        plat_watchdog_pause(g.wd_session, true);
        return false;
    }
    LISA_LOGI(TAG, "resuming the session to VM %s", g.vm_id);
    return true;
}

/* ---- credentials ---- */

static void note_vm_credentials_refused(void)
{
    g.refusals++;
    g.last_refresh_ms = plat_uptime_ms();
    LISA_LOGW(TAG, "VM credentials refused %u time(s) in a row; next refresh no sooner than %us", g.refusals,
              (unsigned)(link_auth_refresh_interval_ms(g.refusals) / 1000));
}

static void note_vm_credentials_accepted(void)
{
    if (g.refusals > 0) {
        LISA_LOGI(TAG, "VM credentials accepted; refresh backoff reset");
    }
    g.refusals = 0;
    g.last_refresh_ms = 0;
}

/*
 * The stored access token was refused: refresh the pair with the refresh token
 * (adapted from upstream app.c ensure_access_token_ready_with_gate_held). With
 * allow_stale, a refused refresh keeps the old pair; without, it means the
 * pairing is gone and it is cleared.
 */
static bool refresh_access_token(link_creds_t *creds, bool allow_stale)
{
    vm_device_tokens_t tokens = { 0 };
    vm_token_refresh_method_t method = VM_TOKEN_REFRESH_NONE;
    /* NULL access token: it was just refused, presenting it again cannot succeed (vm_api.h). */
    int rc = vm_api_refresh_device_token(NULL, creds->refresh, gadget_node_id(), &tokens, &method);
    bool ok = false;
    if (rc == 0) {
        if (strlen(tokens.access_token) < sizeof(creds->access) && strlen(tokens.refresh_token) < sizeof(creds->refresh)) {
            strcpy(creds->access, tokens.access_token);
            strcpy(creds->refresh, tokens.refresh_token);
            ok = link_creds_save(creds) == 0;
            LISA_LOGI(TAG, "device token refresh stored (method=%d)", (int)method);
        } else {
            LISA_LOGE(TAG, "refreshed tokens too long to store");
        }
    } else if (rc == VM_API_ERR_AUTH && !allow_stale) {
        LISA_LOGW(TAG, "device token refresh: device credentials rejected; clearing pairing");
        link_creds_clear();
    } else {
        LISA_LOGW(TAG, "device token refresh failed (%d)", rc);
        ok = allow_stale;
    }
    vm_device_tokens_free(&tokens);
    return ok;
}

/* Adapted from upstream app.c fetch_vms_with_refresh_with_gate_held. */
static int fetch_vms_with_refresh(link_creds_t *creds, vm_info_t *vms, int max)
{
    plat_watchdog_at(g.wd_keeper, "fetch_vms");
    int count = vm_api_fetch_vms(creds->access, vms, max);
    if (count >= 0) {
        note_vm_credentials_accepted();
    }
    if (count != VM_API_ERR_AUTH) {
        return count;
    }
    LISA_LOGW(TAG, "VM fetch rejected token; forcing refresh");
    plat_watchdog_at(g.wd_keeper, "token_refresh");
    if (!refresh_access_token(creds, false)) {
        return VM_API_ERR_AUTH;
    }
    plat_watchdog_at(g.wd_keeper, "fetch_vms");
    count = vm_api_fetch_vms(creds->access, vms, max);
    if (count == VM_API_ERR_AUTH) {
        /* Not a revocation: only the refresh token being refused proves that (upstream app.c). */
        LISA_LOGW(TAG, "VM fetch still rejected after refresh; leaving pairing intact and retrying later");
        note_vm_credentials_refused();
    } else if (count >= 0) {
        note_vm_credentials_accepted();
    }
    return count;
}

/* Adapted from upstream app.c connect_preferred_vm_with_gate_held (no saved vm_url: the default VM). */
static bool connect_preferred_vm(link_creds_t *creds)
{
    set_state(IMPO_LINK_FETCHING);
    vm_info_t vms[VM_API_MAX_VMS];
    int count = fetch_vms_with_refresh(creds, vms, VM_API_MAX_VMS);
    if (count <= 0) {
        return false;
    }
    const vm_info_t *target = vm_find_default(vms, count);
    bool ok = target != NULL;
    if (ok) {
        LISA_LOGI(TAG, "connecting to VM %s", target->vm_name && *target->vm_name ? target->vm_name : target->vm_url);
        ok = connect_vm_target(target);
    }
    vm_list_free(vms, count);
    return ok;
}

/* ---- the keeper task ---- */

static void keeper_task(void *arg)
{
    (void)arg;
    uint64_t no_ip_since = 0, next_try = 0;
    uint32_t backoff = LINK_RETRY_MIN_MS;
    bool resume = false;
    /* The pairing, read again whenever it is saved or cleared (link_creds_generation). On this
     * task's stack, which is in PSRAM: static data lands in the scarce internal SRAM. */
    link_creds_t creds;
    unsigned creds_gen = link_creds_generation() - 1;
    bool paired = false;
    for (;;) {
        plat_watchdog_at(g.wd_keeper, "idle");
        vTaskDelay(pdMS_TO_TICKS(KEEPER_TICK_MS));
        uint64_t now = plat_uptime_ms();

        if (!plat_wifi_has_ip()) {
            if (no_ip_since == 0) {
                no_ip_since = now;
            } else if (now - no_ip_since >= NO_IP_MS && noise_ctrl_is_running()) {
                stop_session("wifi lost");
                resume = true;
                next_try = 0;
            }
            continue;
        }
        no_ip_since = 0;
        if (!plat_time_synced()) {
            continue;   /* certificate checks need the clock */
        }

        if (g.unpaired) {
            g.unpaired = false;
            stop_session("unpaired by Impo");
            link_creds_clear();
            resume = false;
        }
        if (link_creds_generation() != creds_gen) {
            creds_gen = link_creds_generation();
            paired = link_creds_load(&creds) == 0;
        }
        if (!paired) {
            stop_session("not paired");
            set_state(IMPO_LINK_UNPAIRED);
            resume = false;
            next_try = 0;
            continue;
        }
        if (g.drop) {
            g.drop = false;
            stop_session("debug drop");
            resume = true;
            next_try = 0;
        }
        if (g.rename) {
            /* Adapted from upstream app.c device.set_name: the next link.register carries the name. */
            g.rename = false;
            vTaskDelay(pdMS_TO_TICKS(RENAME_DELAY_MS));
            noise_ctrl_set_display_name(gadget_display_name());
            stop_session("renamed");
            resume = true;
            next_try = 0;
        }
        if (noise_ctrl_is_connected() && esp_compat_tls_rx_quiet_ms() >= SILENT_MS) {
            stop_session("gateway silent");
            resume = true;
            next_try = 0;
        }
        if (g.refresh_needed) {
            /* Adapted from upstream app.c schedule_vm_auth_refresh: a fresh fetch_vms, paced. */
            g.refresh_needed = false;
            if (g.last_refresh_ms == 0 || now - g.last_refresh_ms >= link_auth_refresh_interval_ms(g.refusals)) {
                g.last_refresh_ms = now;
                LISA_LOGI(TAG, "refreshing VM credentials after the session's auth rejection");
                resume = false;
                if (connect_preferred_vm(&creds)) {
                    backoff = LINK_RETRY_MIN_MS;
                }
            }
        }

        if (noise_ctrl_is_running() || now < next_try) {
            continue;
        }
        bool ok = resume && resume_session();
        resume = false;
        ok = ok || connect_preferred_vm(&creds);
        if (ok) {
            backoff = LINK_RETRY_MIN_MS;
        } else {
            set_state(IMPO_LINK_IDLE);
            LISA_LOGW(TAG, "no VM session; retrying in %us", (unsigned)(backoff / 1000));
            next_try = now + backoff;
            backoff = link_retry_next_ms(backoff);
        }
    }
}

/* ---- impo_link.h ---- */

int impo_link_start(void)
{
    g.wd_keeper = plat_watchdog_register("impo_link", KEEPER_WDT_MS);
    g.wd_session = plat_watchdog_register("noise_ctrl", SESSION_WDT_MS);
    plat_watchdog_pause(g.wd_session, true);
    esp_compat_wdt_supervise("noise_ctrl", g.wd_session);

    const char *product = gadget_board_get()->product_id;
    if (!link_product_id_ok(product)) {
        LISA_LOGE(TAG, "board product_id \"%s\" is not 2 to 40 of a-z, 0-9 and -; not sent", product);
        product = "";
    }
    esp_compat_set_product_id(product);
    if (product != NULL && product[0] != '\0') {
        LISA_LOGI(TAG, "registers as a custom gadget of product %s", product);
    } else {
        LISA_LOGI(TAG, "registers as a custom gadget without a product (not set up)");
    }

    vm_api_set_base_url(CONFIG_GADGET_API_URL);
    noise_ctrl_init(gadget_node_id(), gadget_display_name(), on_status);
    noise_ctrl_set_display_name(gadget_display_name());
    noise_ctrl_set_command_cb(on_command);
    noise_ctrl_set_host(CONFIG_GADGET_NOISE_HOST);
    impo_link_streams_init();
    return xTaskCreate(keeper_task, "impo_link", KEEPER_STACK_BYTES / sizeof(StackType_t), NULL, 4, NULL) == pdPASS
               ? 0
               : -1;
}

impo_link_state_t impo_link_state(void)
{
    return g.state;
}

int impo_link_send_event(const char *name, cJSON *data)
{
    if (!noise_ctrl_send_event(name, data)) {
        return -1;
    }
    LISA_LOGI(TAG, "event %s queued", name);
    return 0;
}

void impo_link_request_reregister(void)
{
    g.rename = true;
}

void impo_link_debug_drop(void)
{
    g.drop = true;
}

int impo_link_describe(char *buf, size_t cap)
{
    char last[40];
    esp_compat_tls_last_event(last, sizeof(last));
    int n = snprintf(buf, cap, "state=%s node_id=%s vm=%s sessions=%u invokes=%u uptime=%us",
                     state_name(g.state), gadget_node_id(), g.vm_id, (unsigned)g.sessions, (unsigned)g.invokes,
                     (unsigned)(plat_uptime_ms() / 1000));
    if (n > 0 && (size_t)n < cap && noise_ctrl_is_connected()) {
        n += snprintf(buf + n, cap - (size_t)n, "\n  online %us, last rx %u ms ago",
                      (unsigned)((plat_uptime_ms() - g.up_at_ms) / 1000), (unsigned)esp_compat_tls_rx_quiet_ms());
    }
    if (n > 0 && (size_t)n < cap) {
        n += link_history_format(&g.history, buf + n, cap - (size_t)n);
    }
    return n;
}
