/*
 * Copyright (c) Impo contributors.
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

/*
 * The pads are on touch channels 1 (top, GPIO1), 2 and 3 (the sides, GPIO2
 * and GPIO3), as the vendor's factory demo reads them. The chip tracks each
 * channel's untouched level (its benchmark) and raises active/inactive
 * events when the reading moves a fraction of it; those fractions are the
 * demo's. Events land in s_state from the touch interrupt, for poll_buttons.
 */
#include "sparkbot_touch.h"

#include <stdatomic.h>

#include "driver/touch_sens.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "sparkbot.touch";

#define SCAN_TIMEOUT_MS 2000
#define INIT_SCANS 3

static const struct {
    int channel;
    unsigned bit;
    float ratio;    /* active threshold, as a fraction of the benchmark */
} PADS[] = {
    { 1, SPARKBOT_TOUCH_TOP, 0.035f },
    { 2, SPARKBOT_TOUCH_LEFT, 0.08f },
    { 3, SPARKBOT_TOUCH_RIGHT, 0.08f },
};
#define PAD_COUNT (sizeof(PADS) / sizeof(PADS[0]))

static touch_sensor_handle_t s_sensor;
static touch_channel_handle_t s_channel[PAD_COUNT];
static atomic_uint s_state;

static unsigned bit_of(int chan_id)
{
    for (size_t i = 0; i < PAD_COUNT; i++) {
        if (PADS[i].channel == chan_id) {
            return PADS[i].bit;
        }
    }
    return 0;
}

static bool on_active(touch_sensor_handle_t sensor, const touch_active_event_data_t *event, void *ctx)
{
    (void)sensor;
    (void)ctx;
    atomic_fetch_or(&s_state, bit_of(event->chan_id));
    return false;
}

static bool on_inactive(touch_sensor_handle_t sensor, const touch_inactive_event_data_t *event, void *ctx)
{
    (void)sensor;
    (void)ctx;
    atomic_fetch_and(&s_state, ~bit_of(event->chan_id));
    return false;
}

esp_err_t sparkbot_touch_init(void)
{
    touch_sensor_sample_config_t sample = TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2);
    touch_sensor_config_t cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, &sample);
    ESP_RETURN_ON_ERROR(touch_sensor_new_controller(&cfg, &s_sensor), TAG, "controller");
    touch_channel_config_t chan = {
        .active_thresh = { 2000 },   /* until the benchmark is known */
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };
    for (size_t i = 0; i < PAD_COUNT; i++) {
        ESP_RETURN_ON_ERROR(touch_sensor_new_channel(s_sensor, PADS[i].channel, &chan, &s_channel[i]), TAG, "channel %d", PADS[i].channel);
    }
    touch_sensor_filter_config_t filter = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_RETURN_ON_ERROR(touch_sensor_config_filter(s_sensor, &filter), TAG, "filter");

    /* Measure the untouched pads, then set each threshold from its level. */
    ESP_RETURN_ON_ERROR(touch_sensor_enable(s_sensor), TAG, "enable");
    for (int i = 0; i < INIT_SCANS; i++) {
        ESP_RETURN_ON_ERROR(touch_sensor_trigger_oneshot_scanning(s_sensor, SCAN_TIMEOUT_MS), TAG, "scan");
    }
    ESP_RETURN_ON_ERROR(touch_sensor_disable(s_sensor), TAG, "disable");
    for (size_t i = 0; i < PAD_COUNT; i++) {
        uint32_t benchmark = 0;
        ESP_RETURN_ON_ERROR(touch_channel_read_data(s_channel[i], TOUCH_CHAN_DATA_TYPE_BENCHMARK, &benchmark), TAG, "benchmark");
        chan.active_thresh[0] = (uint32_t)(benchmark * PADS[i].ratio);
        ESP_LOGI(TAG, "pad %d: benchmark %" PRIu32 ", threshold %" PRIu32, PADS[i].channel, benchmark, chan.active_thresh[0]);
        ESP_RETURN_ON_ERROR(touch_sensor_reconfig_channel(s_channel[i], &chan), TAG, "threshold");
    }

    const touch_event_callbacks_t callbacks = { .on_active = on_active, .on_inactive = on_inactive };
    ESP_RETURN_ON_ERROR(touch_sensor_register_callbacks(s_sensor, &callbacks, NULL), TAG, "callbacks");
    ESP_RETURN_ON_ERROR(touch_sensor_enable(s_sensor), TAG, "enable");
    return touch_sensor_start_continuous_scanning(s_sensor);
}

unsigned sparkbot_touch_state(void)
{
    return atomic_load(&s_state);
}
