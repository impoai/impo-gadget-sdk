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

/*
 * User settings, persisted in NVS. Setters save immediately and then notify the
 * change listener (from the caller's task), which applies the setting to the
 * hardware. Brightness, auto-sleep and the speaker are polled where they're used.
 */

#define IMPO_SSID_MAX 32
#define IMPO_PASS_MAX 64
#define IMPO_HOST_MAX 63
#define IMPO_VM_MAX 63
#define IMPO_TOKEN_MAX 1023

#define IMPO_MIC_GAIN_MAX 36      /* dB; ES7210 PGA, applied in 3 dB steps */

typedef enum {
    IMPO_SETTING_VOLUME,
    IMPO_SETTING_SPEAKER,
    IMPO_SETTING_MIC_GAIN,
    IMPO_SETTING_BRIGHTNESS,
    IMPO_SETTING_SLEEP,
    IMPO_SETTING_WIFI,          /* on/off or credentials */
    IMPO_SETTING_BLE,
    IMPO_SETTING_HATCH,
} impo_setting_t;

typedef void (*impo_setting_cb_t)(impo_setting_t what);

esp_err_t impo_settings_init(void);
void impo_settings_set_listener(impo_setting_cb_t cb);

int impo_settings_volume(void);         /* 0..100 */
bool impo_settings_speaker_on(void);    /* off: replies are shown, not played */
int impo_settings_mic_gain(void);       /* dB, 0..IMPO_MIC_GAIN_MAX */
int impo_settings_brightness(void);     /* 10..100 */
int impo_settings_sleep_s(void);        /* 0 = never */
bool impo_settings_wifi_on(void);
bool impo_settings_ble_on(void);

void impo_settings_wifi(char ssid[IMPO_SSID_MAX + 1], char pass[IMPO_PASS_MAX + 1]);
void impo_settings_hatch_host(char out[IMPO_HOST_MAX + 1]);
void impo_settings_hatch_vm(char out[IMPO_VM_MAX + 1]);
void impo_settings_hatch_token(char out[IMPO_TOKEN_MAX + 1]);
size_t impo_settings_hatch_token_len(void);

void impo_settings_set_volume(int pct);
void impo_settings_set_speaker_on(bool on);
void impo_settings_set_mic_gain(int db);
void impo_settings_set_brightness(int pct);
void impo_settings_set_sleep_s(int secs);
void impo_settings_set_wifi_on(bool on);
void impo_settings_set_ble_on(bool on);
/* A network name is remembered first among the saved ones and joined now;
 * an empty ssid forgets every saved network. */
void impo_settings_set_wifi(const char *ssid, const char *pass);
void impo_settings_set_hatch_host(const char *host);
void impo_settings_set_hatch_vm(const char *vm);
/* append=true adds to the stored token (for chunked BLE writes). */
esp_err_t impo_settings_set_hatch_token(const char *token, bool append);
