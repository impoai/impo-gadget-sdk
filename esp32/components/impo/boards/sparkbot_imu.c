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
#include "sparkbot_imu.h"

#include <math.h>
#include <string.h>

#include "bmi270.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "impo_link.h"
#include "impo_state.h"

static const char *TAG = "sparkbot.imu";

/* Below this much of g along one axis the gadget is somewhere in between. */
#define LEVEL_G 0.75f
/* Turning faster than this, or accelerating beyond gravity by this much, counts as moving. */
#define MOVING_DPS 40.0f
#define MOVING_G 0.25f

static bmi270_handle_t *s_imu;

/*
 * Which way the chip's axes point in the head, found with the gadget on its
 * base: gravity reads along each as listed. `up` is the sign gravity has on
 * that axis when the named side faces up.
 */
static const struct {
    int axis;
    float up;
    const char *name;
} SIDES[] = {
    { 2, 1.0f, "upright" },      { 2, -1.0f, "upside_down" },
    { 1, 1.0f, "face_up" },      { 1, -1.0f, "face_down" },
    { 0, 1.0f, "on_left_side" }, { 0, -1.0f, "on_right_side" },
};

esp_err_t sparkbot_imu_init(i2c_master_bus_handle_t bus)
{
    /* A BMI260 at 0x68 on the units seen so far; the other address in case. */
    const bmi270_driver_config_t cfg = {
        .addr = BMI270_I2C_ADDRESS_L,
        .interface = BMI270_USE_I2C,
        .i2c_bus = bus,
    };
    esp_err_t err = bmi270_create(&cfg, &s_imu);
    if (err != ESP_OK) {
        bmi270_driver_config_t high = cfg;
        high.addr = BMI270_I2C_ADDRESS_H;
        err = bmi270_create(&high, &s_imu);
    }
    if (err != ESP_OK) {
        s_imu = NULL;
        ESP_LOGW(TAG, "no BMI270: %s", esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }
    const bmi270_config_t run = {
        .acce_odr = BMI270_ACC_ODR_100_HZ,
        .acce_range = BMI270_ACC_RANGE_4_G,
        .gyro_odr = BMI270_GYR_ODR_100_HZ,
        .gyro_range = BMI270_GYR_RANGE_1000_DPS,
    };
    ESP_RETURN_ON_ERROR(bmi270_start(s_imu, &run), TAG, "start");
    return ESP_OK;
}

esp_err_t sparkbot_imu_read(sparkbot_imu_reading_t *out)
{
    if (!s_imu) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(bmi270_get_acce_data(s_imu, &out->accel[0], &out->accel[1], &out->accel[2]), TAG, "accel");
    ESP_RETURN_ON_ERROR(bmi270_get_gyro_data(s_imu, &out->gyro[0], &out->gyro[1], &out->gyro[2]), TAG, "gyro");

    out->orientation = "tilted";
    for (size_t i = 0; i < sizeof(SIDES) / sizeof(SIDES[0]); i++) {
        if (out->accel[SIDES[i].axis] * SIDES[i].up >= LEVEL_G) {
            out->orientation = SIDES[i].name;
            break;
        }
    }
    float a = sqrtf(out->accel[0] * out->accel[0] + out->accel[1] * out->accel[1] + out->accel[2] * out->accel[2]);
    float w = fmaxf(fabsf(out->gyro[0]), fmaxf(fabsf(out->gyro[1]), fabsf(out->gyro[2])));
    out->moving = fabsf(a - 1.0f) > MOVING_G || w > MOVING_DPS;
    return ESP_OK;
}

/* ---- Events ---- */

#define WATCH_MS 100                 /* sample interval */
#define SHAKE_G 0.6f                 /* beyond gravity, on one sample */
#define SHAKE_SAMPLES 3              /* within a second: a shake, not a bump */
#define SHAKE_QUIET_MS 8000          /* one "shaken" per this long */
#define MOVE_SETTLE_MS 1500          /* off upright (or back) this long before it counts */
#define MOVE_QUIET_MS 5000           /* one pick-up or put-down per this long */

static void watch_task(void *arg)
{
    (void)arg;
    int hits = 0, window = 0;
    TickType_t last_shake = 0, last_move = 0, changed_at = 0;
    bool upright = true, reported_upright = true;
    float peak = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WATCH_MS));
        sparkbot_imu_reading_t r;
        if (sparkbot_imu_read(&r) != ESP_OK) {
            continue;
        }
        TickType_t now = xTaskGetTickCount();
        float a = sqrtf(r.accel[0] * r.accel[0] + r.accel[1] * r.accel[1] + r.accel[2] * r.accel[2]);
        float excess = fabsf(a - 1.0f);

        /* Shaken: several hard samples within a second. */
        if (excess > SHAKE_G) {
            hits++;
            peak = fmaxf(peak, excess);
        }
        if (++window >= 1000 / WATCH_MS) {
            if (hits >= SHAKE_SAMPLES && now - last_shake > pdMS_TO_TICKS(SHAKE_QUIET_MS)) {
                last_shake = now;
                cJSON *data = cJSON_CreateObject();
                cJSON_AddNumberToObject(data, "peak_g", round(peak * 10) / 10);
                cJSON_AddStringToObject(data, "strength", peak > 2.0f ? "hard" : "gentle");
                ESP_LOGI(TAG, "shaken (%.1f g)", peak);
                impo_state_poke();
                impo_link_send_event("shaken", data);
            }
            hits = 0;
            window = 0;
            peak = 0;
        }

        /* Picked up / put down: upright or not, held for MOVE_SETTLE_MS. */
        bool now_upright = !strcmp(r.orientation, "upright");
        if (now_upright != upright) {
            upright = now_upright;
            changed_at = now;
        }
        if (upright != reported_upright && now - changed_at >= pdMS_TO_TICKS(MOVE_SETTLE_MS)
            && now - last_move > pdMS_TO_TICKS(MOVE_QUIET_MS)) {
            reported_upright = upright;
            last_move = now;
            cJSON *data = cJSON_CreateObject();
            cJSON_AddStringToObject(data, "orientation", r.orientation);
            ESP_LOGI(TAG, "%s (%s)", upright ? "put down" : "picked up", r.orientation);
            impo_state_poke();
            impo_link_send_event(upright ? "put_down" : "picked_up", data);
        }
    }
}

esp_err_t sparkbot_imu_watch_start(void)
{
    if (!s_imu) {
        return ESP_ERR_NOT_FOUND;
    }
    return xTaskCreate(watch_task, "imu_watch", 3072, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
