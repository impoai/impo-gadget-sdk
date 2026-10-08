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
#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The ESP-SparkBot's motion sensor (a BMI260 or BMI270) on the board's I2C
 * bus, for the imu.read command. */

typedef struct {
    float accel[3];     /* g, the sensor's own axes */
    float gyro[3];      /* degrees per second */
    const char *orientation;    /* "upright", "upside_down", "face_up", "face_down",
                                   "on_left_side", "on_right_side" or "tilted" */
    bool moving;        /* being shaken or turned right now */
} sparkbot_imu_reading_t;

/* ESP_ERR_NOT_FOUND when the chip doesn't answer; the command then says so. */
esp_err_t sparkbot_imu_init(i2c_master_bus_handle_t bus);

esp_err_t sparkbot_imu_read(sparkbot_imu_reading_t *out);

#ifdef __cplusplus
}
#endif
