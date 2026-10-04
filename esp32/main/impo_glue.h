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

#include "led_status.h"

// Runs the UI (components/impo) on top of Home Link: Link owns Wi-Fi, BLE,
// setup credentials and the Hatch account; the UI is the avatar, voice and
// settings. Only built with CONFIG_IMPO_ENABLED.

// Before app_run(): registers Impo's BLE service and Link operations.
void impo_glue_start(void);
// From app_run(): NVS and identity are up, so Impo can start its UI.
void impo_glue_storage_ready(void);
// From app_run(): boot Wi-Fi/VM connect is done; Impo may drive the radios.
void impo_glue_link_ready(void);
// The LED backend for Impo builds: Link's status, shown on the display.
void impo_glue_led_state(led_state_t state);
