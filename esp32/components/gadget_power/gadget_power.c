// SPDX-License-Identifier: Apache-2.0
#include "gadget_power.h"
#include <stdatomic.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static atomic_bool s_shutting_down;
static esp_err_t (*s_prepare)(void);
static void (*s_recover)(void);

bool gadget_power_button_update(gadget_power_button_t *state, bool pressed, int64_t now_ms) {
    if (!pressed) {
        state->armed = true;
        state->tracking = false;
    } else if (state->armed) {
        if (!state->tracking) {
            state->tracking = true;
            state->pressed_at_ms = now_ms;
        } else if (now_ms - state->pressed_at_ms >= GADGET_POWER_HOLD_MS) {
            state->armed = false;
            state->tracking = false;
            return true;
        }
    }
    return false;
}

void gadget_power_set_system_hooks(esp_err_t (*prepare)(void), void (*recover)(void)) {
    s_prepare = prepare;
    s_recover = recover;
}

bool gadget_power_is_shutting_down(void) {
    return atomic_load(&s_shutting_down);
}

esp_err_t gadget_power_off(const gadget_power_ops_t *ops) {
    if (!ops || !ops->power_off) return ESP_ERR_INVALID_ARG;
    if (atomic_exchange(&s_shutting_down, true)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = s_prepare ? s_prepare() : ESP_OK;
    if (err == ESP_OK) {
        if (ops->show_off) ops->show_off();
        if (ops->button_pressed) {
            // A still-held wake button must not immediately undo shutdown.
            // Two released samples also reject a single bouncing sample.
            unsigned released = 0;
            while (released < 2) {
                released = ops->button_pressed() ? 0 : released + 1;
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        err = ops->power_off();
        if (err == ESP_OK) err = ESP_FAIL; // A successful power-off cannot return.
        if (s_recover) s_recover();
    }
    atomic_store(&s_shutting_down, false);
    return err;
}
