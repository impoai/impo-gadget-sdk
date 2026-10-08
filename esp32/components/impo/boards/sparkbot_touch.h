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

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The ESP-SparkBot's three capacitive pads: one across the top of the head
 * (two foils on one channel) and one on each side. The top is the talk
 * button, either side the aux button.
 */

#define SPARKBOT_TOUCH_TOP   (1u << 0)
#define SPARKBOT_TOUCH_LEFT  (1u << 1)
#define SPARKBOT_TOUCH_RIGHT (1u << 2)

esp_err_t sparkbot_touch_init(void);

/* The pads touched right now, SPARKBOT_TOUCH_* bits. */
unsigned sparkbot_touch_state(void);

#ifdef __cplusplus
}
#endif
