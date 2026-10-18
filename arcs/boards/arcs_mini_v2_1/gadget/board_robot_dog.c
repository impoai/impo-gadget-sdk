/*
 * ARCS-MINI V2.1 robot dog: the board file (gadget_board.h, docs/BOARD_INTERFACE.md).
 *
 * Hardware (boards/arcs_mini_v2_1/pinmux.h, mtt1.0-impo V2.1 drivers):
 *   4 leg servos PA04-PA07 (pwm0 ch4-7), 5-pixel WS2812 on PA08 (eyes = 2, 4),
 *   TTP223 touch PA09 / PA13, MPU6050 on I2C0 (PB06/PB07), power key PB04,
 *   battery ADC PB05, TP4056 CHRG PB08, USB detect PB00, power latch PB03,
 *   speaker amp enable PA01, one mic on the codec's left ADC. No display.
 */
#include <stddef.h>

#include "lisa_gpio.h"
#include "pinmux.h"

#include "gadget_board.h"
#include "dog_board.h"
#include "dog_motion.h"

static lisa_device_t *s_amp_gpio;

/* Speaker amplifier enable on PA01, active high. */
static void set_amp(bool on)
{
    if (s_amp_gpio != NULL) {
        lisa_gpio_write_pin(s_amp_gpio, PA_EN_PIN, on ? LISA_GPIO_HIGH : LISA_GPIO_LOW);
    }
}

static int init(void)
{
    s_amp_gpio = lisa_device_get(PA_EN_DEVICE_NAME);
    if (lisa_device_ready(s_amp_gpio)) {
        lisa_gpio_configure(s_amp_gpio, PA_EN_PIN, LISA_GPIO_OUTPUT | LISA_GPIO_OUTPUT_INIT_LOW);
    } else {
        s_amp_gpio = NULL;
    }
    dog_power_init();
    int r = dog_motion_init();
    if (r == 0) {
        r = dog_commands_init();
    }
    dog_light_start();
    return r;
}

static const char s_instructions[] =
    "This gadget is a small four-legged robot dog, about 15 cm long, on a desk or the floor. "
    "People talk to it by touching its back; your replies are spoken aloud by its small speaker, so "
    "answer in one to three short sentences, no markdown, in the language you were spoken to. "
    "It walks, turns, sits, lies down and does tricks: use its commands when the person asks it to "
    "move or play, and feel free to add a small trick that fits what you say (a wiggle when happy). "
    "Motions return at once and run for a few seconds; chassis.status tells when they end. "
    "Keep it away from desk edges when walking. It cannot see. "
    "When it is picked up, greet them warmly. When it is put down, a short word is enough. "
    "When it is shaken, say it feels dizzy. When it is turned upside down, it can wave its legs "
    "(dog.trick upside_wiggle); ask whether to roll it back over (dog.trick handstand). "
    "Its eye light shows its state by itself; chassis.set_light sets a colour or effect when asked, "
    "and display.show_animation gives the light back.";

static gadget_board_t s_board = {
    .name = "ARCS Robot Dog",
    .model_id = "arcs-mini-v2.1-dog",
    .ble_prefix = "ImpoGadget",
    /* The dog's product on the Impo developer platform: its manifest (what the dog is for,
     * its commands and events, from dog_events and s_instructions here) reaches the agent. */
    .product_id = "b4066283-da1a-4c2f-922a-bfa1ad4804e9",
    .init = init,
    .audio = {
        .speaker = true,
        .microphone = true,
        .mic_channel = 0,
        .mic_gain_db = 30,
        .set_amp = set_amp,
    },
    .poll_buttons = dog_poll_buttons,
    .read_power = dog_read_power,
    .power_off = dog_power_off,
    .show_status = dog_show_status,
    .show_animation = dog_show_animation,
    .commands = dog_commands,
    .features = "motion_sensor,touch_pads,locomotion,lights",
    .instructions = s_instructions,
};

const gadget_board_t *gadget_board_get(void)
{
    /* Table sizes live in dog_commands.c. */
    s_board.command_count = dog_command_count;
    s_board.events = dog_events;
    s_board.event_count = dog_event_count;
    return &s_board;
}
