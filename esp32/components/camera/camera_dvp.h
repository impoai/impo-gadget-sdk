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

#include "camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A parallel (DVP) sensor on the chip's camera interface, driven by
 * esp32-camera: the ESP-SparkBot's OV2640 or GC2145. A sensor that can't
 * make JPEG itself hands over RGB565, encoded here. The sensor is started at
 * the first photo and left running (see camera_dvp.c). It can't stream.
 */

typedef struct {
    const char *name;               /* camera_driver_t.name */
    int xclk, pclk, vsync, href;    /* GPIOs */
    int d[8];                       /* D0..D7 */
    int i2c_port;                   /* an I2C bus the board has already opened (SCCB) */
    bool vflip, hmirror;            /* how the module is mounted */
} camera_dvp_config_t;

/* The backend for `config` (copied), to pass to camera_register(). One per device. */
const camera_driver_t *camera_dvp(const camera_dvp_config_t *config);

#ifdef __cplusplus
}
#endif
