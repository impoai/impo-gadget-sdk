// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
// Call before Link initialization so battery-powered revisions latch on.
bool xteink_x4_init(void);
bool xteink_x4_confirm_pressed(void);
// Queues text for the display worker; false while an image is downloading.
bool xteink_x4_show_text(const char *text);
// Reserve the canvas for an asynchronous image. Release on start failure.
bool xteink_x4_begin_image(bool preserve, bool refresh);
void xteink_x4_cancel_image(void);
// Runs on a worker task, never on the network callback. Persists a snapshot.
esp_err_t xteink_x4_save_power_off_image(bool reset);
#ifdef __cplusplus
}
#endif
