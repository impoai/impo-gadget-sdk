/*
 * SDK button policy: polls the board's buttons every 10 ms. Holding the main
 * button for 3 s powers the gadget off. A press already held at boot (the
 * power-on press) is ignored until it is released.
 */
#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "platform_arcs.h"
#include "gadget_board.h"
#include "gadget_core.h"

#define TAG "gadget_in"
#include "lisa_log.h"

#define POLL_MS        10
#define POWER_HOLD_MS  3000
#define WATCHDOG_MS    5000
#define AUX_SETTLE_MS  3000   /* touch sensors report a touch while they calibrate at power-on */   /* the power key must always work; power_off may take a moment */

static void (*s_on_aux)(void);

void gadget_input_on_aux(void (*cb)(void))
{
    s_on_aux = cb;
}

static void input_task(void *arg)
{
    const gadget_board_t *b = arg;
    bool pressed = false;
    bool armed = false;          /* false only while the power-on press is still held */
    bool first = true;
    uint32_t held_ms = 0;
    int wd = plat_watchdog_register("gadget_in", WATCHDOG_MS);
    for (;;) {
        plat_watchdog_checkin(wd);
        unsigned e = b->poll_buttons();
        if (first) {
            /* The first poll reports a press only when the key is already down at boot. */
            armed = !(e & GADGET_BTN_MAIN_PRESS);
            first = false;
        }
        if (e & GADGET_BTN_MAIN_PRESS) {
            LISA_LOGI(TAG, "main button down%s", armed ? "" : " (power-on press, ignored)");
            pressed = true;
            held_ms = 0;
        }
        if (e & GADGET_BTN_MAIN_RELEASE) {
            LISA_LOGI(TAG, "main button up after %u ms", (unsigned)held_ms);
            pressed = false;
            armed = true;
        }
        if ((e & GADGET_BTN_AUX_PRESS) && plat_uptime_ms() < AUX_SETTLE_MS) {
            LISA_LOGI(TAG, "aux button down at boot, ignored");
        } else if (e & GADGET_BTN_AUX_PRESS) {
            LISA_LOGI(TAG, "aux button down");
            if (s_on_aux != NULL) {
                s_on_aux();
            }
        }
        if (pressed) {
            held_ms += POLL_MS;
            if (armed && held_ms == POWER_HOLD_MS && b->power_off != NULL) {
                LISA_LOGW(TAG, "main button held %u ms: powering off", (unsigned)held_ms);
                b->power_off();
                LISA_LOGW(TAG, "power off did not happen (USB power keeps the board on?)");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

int gadget_input_start(void)
{
    const gadget_board_t *b = gadget_board_get();
    if (b->poll_buttons == NULL) {
        return 0;
    }
    return xTaskCreate(input_task, "gadget_in", 2048 / sizeof(StackType_t), (void *)b, 5, NULL) == pdPASS ? 0 : -1;
}
