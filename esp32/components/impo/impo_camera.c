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
#include "impo_camera.h"

#include <stdlib.h>

#include "camera.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#if CONFIG_IMPO_WATCHER_CAMERA
#include "boards/watcher_camera.h"
#endif

const char *impo_camera_name(void)
{
    const camera_driver_t *d = camera_get();
    return d ? d->name : NULL;
}

char *impo_camera_base64(const uint8_t *jpeg, size_t len)
{
    size_t cap = (len + 2) / 3 * 4 + 1, out = 0;
    char *b64 = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (b64 && mbedtls_base64_encode((unsigned char *)b64, cap, &out, jpeg, len) != 0) {
        free(b64);
        return NULL;
    }
    return b64;
}

bool impo_camera_capture(char **jpeg_base64, const char **error)
{
    if (!jpeg_base64 || !error) return false;
    *jpeg_base64 = NULL;
#if CONFIG_IMPO_WATCHER_CAMERA
    return watcher_camera_capture(jpeg_base64, error);
#else
    camera_frame_t frame;
    esp_err_t err = camera_capture(&frame);
    if (err != ESP_OK) {
        *error = err == ESP_ERR_TIMEOUT ? "the camera did not respond"
                 : err == ESP_ERR_NO_MEM ? "camera memory allocation failed"
                 : err == ESP_ERR_NOT_SUPPORTED ? "camera unavailable"
                 : err == ESP_ERR_INVALID_STATE ? "camera busy"
                                                : "camera capture failed";
        return false;
    }
    *jpeg_base64 = impo_camera_base64(frame.jpeg, frame.len);
    camera_release(&frame);
    *error = *jpeg_base64 ? NULL : "camera memory allocation failed";
    return *jpeg_base64 != NULL;
#endif
}
