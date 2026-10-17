// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include "cJSON.h"
#ifdef __cplusplus
extern "C" {
#endif

// A backend owns storage, locking and refresh. Coordinates and text layout
// are shared; no panel pins, network calls or fixed page layout live here.
typedef struct {
    int width, height;
    bool (*begin)(void); // Nonblocking; reserve the canvas until end().
    void (*pixel)(int x, int y, bool white);
    void (*end)(bool refresh);
    bool (*busy)(void);
    int (*power_off_image_state)(void); // -1 unavailable, 0 default, 1 saved.
} display_canvas_ops_t;
void display_canvas_bind(const display_canvas_ops_t *ops);
cJSON *display_canvas_info(void);
void display_canvas_describe(cJSON *commands);
// NULL means this is not a canvas command. Otherwise returns an owned result.
cJSON *display_canvas_command(const char *command, const cJSON *params);
#ifdef __cplusplus
}
#endif
