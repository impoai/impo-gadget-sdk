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
#include <stdint.h>

#include "esp_err.h"

#include "impo_settings.h"

/*
 * Wi-Fi station driven by impo_settings: joins whichever saved network is in
 * range, reconnects with backoff, and runs scans for the settings screen. All
 * calls are safe from any task; status is copied out for the UI to poll.
 */

typedef enum {
    IMPO_WIFI_OFF,
    IMPO_WIFI_NO_NETWORK,     /* on, but nothing saved */
    IMPO_WIFI_CONNECTING,
    IMPO_WIFI_CONNECTED,
    IMPO_WIFI_FAILED,         /* gave up (e.g. wrong password) */
    IMPO_WIFI_NOT_NEARBY,     /* none of the saved networks showed up in a scan; looking again less often */
} impo_wifi_state_t;

typedef struct {
    impo_wifi_state_t state;
    char ssid[IMPO_SSID_MAX + 1];
    char ip[16];
    int rssi;
    char detail[40];          /* human-readable reason while connecting/failed */
} impo_wifi_status_t;

typedef struct {
    char ssid[IMPO_SSID_MAX + 1];
    int8_t rssi;
    bool secure;
} impo_wifi_ap_t;

#define IMPO_WIFI_SAVED_MAX 8
typedef struct {
    char ssid[IMPO_SSID_MAX + 1];
    bool hidden;              /* joined by name; doesn't show in scans */
} impo_wifi_saved_t;

esp_err_t impo_wifi_start(void);
/* Re-reads on/off and credentials from settings and (re)connects. */
void impo_wifi_apply(void);
void impo_wifi_status(impo_wifi_status_t *out);
bool impo_wifi_connected(void);
typedef enum {
    IMPO_WIFI_FULL,   /* the mode from before: no modem sleep, but for BLE coexistence */
    IMPO_WIFI_DOZE,   /* modem sleep between DTIM beacons: a few hundred ms extra latency */
    IMPO_WIFI_REST,   /* over several beacons, and Link's session polls less: up to a second */
} impo_wifi_power_t;
void impo_wifi_power(impo_wifi_power_t level);
/* Leaves Wi-Fi, dropping Link's session and Hatch's, until called with false,
 * which rejoins at once. */
void impo_wifi_nap(bool nap);

esp_err_t impo_wifi_scan(void);
bool impo_wifi_scanning(void);
/* Copies the latest results (strongest first); *gen changes when they do. */
int impo_wifi_scan_results(impo_wifi_ap_t *out, int max, uint32_t *gen);

/* Saved networks, most recently joined first. Returns the count. */
int impo_wifi_saved(impo_wifi_saved_t *out, int max);
/* Forgets one saved network, disconnecting first if it's the one in use. */
void impo_wifi_forget(const char *ssid);
