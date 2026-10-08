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
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The camera.capture Home Link command, for any board that registered a
 * camera (components/camera). The Watcher's live preview wraps this with its
 * own frame (watcher_camera.h); other boards use it as is.
 */

/* The gadget has a camera to offer; its name for the command's description. */
#if CONFIG_IMPO_CAMERA
const char *impo_camera_name(void);
#else
static inline const char *impo_camera_name(void) { return NULL; }
#endif

/* One JPEG from the board's camera as base64, in PSRAM, the caller's to free;
 * or false with a reason. Blocks for the capture, so not on the Link task. */
bool impo_camera_capture(char **jpeg_base64, const char **error);

/* `jpeg` as base64 in PSRAM, or NULL. */
char *impo_camera_base64(const uint8_t *jpeg, size_t len);

#ifdef __cplusplus
}
#endif
