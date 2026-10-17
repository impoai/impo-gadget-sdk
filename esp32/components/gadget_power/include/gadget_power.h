// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GADGET_POWER_HOLD_MS 1500

typedef struct {
    bool armed; // Require release after boot/wake and after a handled hold.
    bool tracking;
    int64_t pressed_at_ms;
} gadget_power_button_t;

// Zero-initialize state. Returns true once for each continuous long press.
bool gadget_power_button_update(gadget_power_button_t *state, bool pressed, int64_t now_ms);

typedef struct {
    void (*show_off)(void);             // Optional; return after the screen is ready.
    bool (*button_pressed)(void);       // Optional; wait for release before powering off.
    esp_err_t (*power_off)(void);       // Board hardware only. Normally never returns.
} gadget_power_ops_t;

// Set once before starting input tasks. prepare quiesces the application;
// recover restores it if hardware power-off returns. No credentials are erased.
void gadget_power_set_system_hooks(esp_err_t (*prepare)(void), void (*recover)(void));
bool gadget_power_is_shutting_down(void);
// Blocking: call only on an input/worker task, never a network or UI callback.
esp_err_t gadget_power_off(const gadget_power_ops_t *ops);

#ifdef __cplusplus
}
#endif
