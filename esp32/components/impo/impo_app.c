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

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "impo_audio.h"
#include "impo_battery.h"
#include "impo_board.h"
#include "impo_ble.h"
#include "impo_chat.h"
#include "impo_input.h"
#include "impo_settings.h"
#include "impo_state.h"
#include "impo_ui.h"
#include "impo_voice.h"
#include "impo_wifi.h"

static const char *TAG = "impo";

/* Applies saved settings to hardware; runs in whichever task changed them. */
static void on_setting(impo_setting_t what)
{
    switch (what) {
    case IMPO_SETTING_VOLUME:
        impo_audio_set_volume(impo_settings_volume());
        break;
    case IMPO_SETTING_MIC_GAIN:
        impo_audio_set_mic_gain(impo_settings_mic_gain());
        break;
    case IMPO_SETTING_WIFI:
        impo_wifi_apply();
        break;
    case IMPO_SETTING_BLE:
        impo_ble_apply();
        break;
    case IMPO_SETTING_HATCH:
        impo_hatch_config_changed();
        break;
    default:
        break;   /* brightness, sleep and the speaker are polled where they're used */
    }
}

const impo_board_t *impo_board;

void impo_app_run(const impo_board_t *board)
{
    impo_board = board;
    ESP_LOGI(TAG, "board: %s", board->name);
    ESP_ERROR_CHECK(board->init());
    ESP_ERROR_CHECK(impo_settings_init());
    impo_settings_set_listener(on_setting);
    impo_state_init();
    impo_battery_init();
    impo_state_set_caption("WAKING UP...");
    ESP_ERROR_CHECK(impo_ui_start());
    ESP_LOGI(TAG, "UI built: free internal %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    QueueHandle_t q = xQueueCreate(16, sizeof(impo_input_event_t));
    ESP_ERROR_CHECK(impo_input_start(q));

    /* Let the boot animation (flame ignites, eyes open) play out. */
    vTaskDelay(pdMS_TO_TICKS(1400));
    if (!board->audio_init) {
        ESP_LOGI(TAG, "display-only board: no microphone or speaker");
        impo_state_set_mode(IMPO_MODE_IDLE);
        impo_state_set_caption("%s", "");
    } else if (impo_voice_start(q) != ESP_OK) {
        ESP_LOGE(TAG, "voice pipeline unavailable");
    } else {
        impo_state_set_mode(IMPO_MODE_IDLE);
        impo_state_set_caption("%s", "");   /* the button icons say how to talk */
    }

    impo_hatch_start();
    /* Home Link owns the radios; these just hand it the saved settings. */
    impo_wifi_apply();
    impo_ble_apply();
    ESP_LOGI(TAG, "ready: free heap %u internal, %u psram",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
