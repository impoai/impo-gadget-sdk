/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "impo_wifi.h"

/*
 * Impo runs inside Home Link, which owns the radios, the setup credentials and
 * the Hatch account. Link registers these operations at boot
 * (main/impo_glue.c); Impo's Wi-Fi status, scans, saved networks, BLE and Hatch
 * VM lookups all go through them. Every call is safe from any task.
 */

typedef enum {
    IMPO_LINK_BOOT,
    IMPO_LINK_UNPAIRED,       /* advertising for the Impo app */
    IMPO_LINK_PAIRING,        /* the app is connected */
    IMPO_LINK_CONFIRM,        /* waiting for a button press to confirm pairing */
    IMPO_LINK_CONNECTING,     /* joining Wi-Fi / reaching the VM */
    IMPO_LINK_ONLINE,
    IMPO_LINK_OFFLINE,
    IMPO_LINK_ERROR,
} impo_link_state_t;

/* A reply frame of a request on Link's session: see noise_ctrl_req_cb in main/noise_control.h.
 * IMPO_LINK_REQ_TOO_LARGE in place of -1: a reply frame too big for this board's
 * buffers ended the session (NOISE_CTRL_REQ_TOO_LARGE). */
#define IMPO_LINK_REQ_TOO_LARGE (-2)
typedef void (*impo_link_req_cb)(void *ctx, int status, const uint8_t *data, size_t len, bool end);

typedef struct {
    void (*wifi_status)(impo_wifi_status_t *out);
    void (*wifi_apply)(void);                     /* on/off or credentials changed */
    void (*wifi_get)(char *ssid, char *pass);     /* IMPO_SSID_MAX+1 / IMPO_PASS_MAX+1; pass may be NULL */
    void (*wifi_set)(const char *ssid, const char *pass);
    esp_err_t (*wifi_scan)(void);
    bool (*wifi_scanning)(void);
    int (*wifi_scan_results)(impo_wifi_ap_t *out, int max, uint32_t *gen);
    void (*ble_apply)(void);                      /* BLE phone setup setting changed */
    bool (*ble_started)(void);
    bool (*hatch_linked)(void);                   /* paired to a Hatch account */
    /* Fills the VM whose id is want_vm (empty: Link's preferred VM). *vm_token is heap. */
    bool (*hatch_vm)(const char *want_vm, char *vm_id, size_t id_cap, char *vm_name, size_t name_cap,
                     char **vm_token);
    bool (*talk_press)(void);                     /* true: Link used the press (pairing confirm) */
    void (*reset_setup)(void);                    /* forget Link setup and reboot */
    /* Requests to the VM on Link's own Noise session, for boards without PSRAM for their own. */
    bool (*req_ready)(void);                      /* the session is up */
    int64_t (*req_open)(const char *verb, const char *path, const char *const *headers, bool end_body,
                        impo_link_req_cb cb, void *ctx);
    bool (*req_send)(int64_t id, const void *data, size_t len, bool end_body, int wait_ms);
    void (*req_cancel)(int64_t id);
    void (*power_save)(bool on);                  /* screen off: the session polls less */
    void (*wifi_nap)(bool nap);                   /* screen off a while: leave Wi-Fi until false */
    int (*wifi_saved)(impo_wifi_saved_t *out, int max);   /* most recently joined first */
    void (*wifi_forget)(const char *ssid);        /* one saved network; empty forgets them all */
} impo_link_ops_t;

void impo_link_register(const impo_link_ops_t *ops);

/* Link's status, mirrored from its LED state. */
void impo_link_set_state(impo_link_state_t state);
impo_link_state_t impo_link_state(void);
const char *impo_link_state_name(impo_link_state_t state);

bool impo_link_hatch_linked(void);
bool impo_link_hatch_vm(const char *want_vm, char *vm_id, size_t id_cap, char *vm_name, size_t name_cap,
                        char **vm_token);
bool impo_link_talk_press(void);
void impo_link_reset_setup(void);
/* The first saved network, owned by Link. Return false when Link hasn't registered. */
bool impo_link_wifi_get(char *ssid, char *pass);
bool impo_link_wifi_set(const char *ssid, const char *pass);
bool impo_link_ble_started(void);
/* Requests on Link's session (noise_ctrl_req_* in main/noise_control.h); open returns 0 if down. */
bool impo_link_req_ready(void);
int64_t impo_link_req_open(const char *verb, const char *path, const char *const *headers, bool end_body,
                           impo_link_req_cb cb, void *ctx);
bool impo_link_req_send(int64_t id, const void *data, size_t len, bool end_body, int wait_ms);
void impo_link_req_cancel(int64_t id);
void impo_link_ble_apply(void);
