/* SPDX-License-Identifier: MIT
 * ARCS Mini voice box. No actuators are initialized or advertised.
 * PB04 short click starts/stops an SDK voice turn; hold retains SDK power-off.
 */
#include <stddef.h>
#include "lisa_gpio.h"
#include "pinmux.h"
#include "power_manager.h"
#include "platform_arcs.h"
#include "gadget_board.h"
#include "arcs_mini_button.h"
#include "arcs_mini_display.h"
#include "arcs_mini_wake.h"
#include "gadget_voice.h"
#include "impo_arcs_mini.h"
#include "impo_product_config.h"

static lisa_device_t *s_amp, *s_gpio;
static arcs_mini_button_t s_button;
static void set_amp(bool on)
{
    if (s_amp) lisa_gpio_write_pin(s_amp, PA_EN_PIN, on ? LISA_GPIO_HIGH : LISA_GPIO_LOW);
}
static int init(void)
{
    s_amp = lisa_device_get("gpioa");
    s_gpio = lisa_device_get("gpiob");
    if (!lisa_device_ready(s_amp) || !lisa_device_ready(s_gpio)) return -1;
    lisa_gpio_configure(s_amp, PA_EN_PIN, LISA_GPIO_OUTPUT | LISA_GPIO_OUTPUT_INIT_LOW);
    lisa_gpio_configure(s_gpio, POWER_KEY_PIN, LISA_GPIO_CONFIG_INPUT_PULLUP);
    /* A screen failure must not prevent the independent voice path from starting. */
    arcs_mini_display_start();
    gadget_voice_set_continuous(impo_arcs_mini_product()->continuous_dialogue);
    arcs_mini_wake_start();
    const impo_arcs_mini_application_t *app=impo_arcs_mini_application();
    return app && app->on_init ? app->on_init() : 0;
}
static void show_status(gadget_status_t status)
{
    arcs_mini_display_show(status);
    const impo_arcs_mini_application_t *app=impo_arcs_mini_application();
    if (app && app->on_status) app->on_status(status);
}
static unsigned poll_buttons(void)
{
    return arcs_mini_button_poll(&s_button,
        lisa_gpio_read_pin(s_gpio, POWER_KEY_PIN) == LISA_GPIO_LOW,
        (uint32_t)plat_uptime_ms());
}
static int read_power(gadget_power_t *out)
{
    *out = (gadget_power_t){ .usb_power = power_is_usb_plugged() };
    return 0; /* No unverified battery curve is reported. */
}
static int power_off(void) { power_shutdown(); return -1; }
static gadget_board_t s_board = {
    .name = IMPO_DISPLAY_NAME,
    .model_id = IMPO_MODEL_ID,
    .ble_prefix = "ImpoGadget",
    .product_id = IMPO_PLATFORM_PRODUCT_ID,
    .init = init,
    .audio = { .speaker = true, .microphone = true, .mic_channel = 0,
               .mic_gain_db = 30, .set_amp = set_amp },
    .poll_buttons = poll_buttons,
    .read_power = read_power,
    .power_off = power_off,
    .show_status = show_status,
    .show_animation = arcs_mini_display_restore,
    .instructions = IMPO_INSTRUCTIONS,
};
const gadget_board_t *gadget_board_get(void)
{
    /* The official main binds this before starting the gadget tasks. Runtime
     * callers only read the descriptor; never rewrite it from multiple tasks. */
    static bool configured;
    if (!configured) {
        const impo_arcs_mini_application_t *app=impo_arcs_mini_application();
        if (app) {
            s_board.commands=app->commands; s_board.command_count=app->command_count;
            s_board.events=app->events; s_board.event_count=app->event_count;
        }
        configured=true;
    }
    return &s_board;
}
