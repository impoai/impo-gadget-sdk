/*
 * What an Impo gadget needs from ARCS hardware.
 *
 * Mirrors impo-gadget-sdk/esp32/components/impo/impo_board.h. Each board is one
 * file in boards/<board>/ that fills in a gadget_board_t and returns it from
 * gadget_board_get(). Everything else is the SDK and is shared by every ARCS
 * board: Wi-Fi, BLE pairing, the Impo link, the voice loop, the standard
 * commands (CAPABILITIES.md), power and button policy, watchdog, OTA.
 *
 * Unlike ESP32 boards, every ARCS board has the same SoC, so the on-chip audio
 * codec, ADC, PWM, I2C and radios are SDK drivers; a board only says how its
 * parts are wired and adds what only it has.
 *
 * A hook a board does not fill is hardware it does not have: the SDK then
 * leaves out the matching capabilities and commands.
 *
 * Implemented by gadget/ (SDK) and boards/arcs_mini_v2_1 (the robot dog).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Commands, events (same shape as impo_commands.h / impo_board.h) ---- */

/*
 * A command only this board has. `required` / `optional` are JSON objects of
 * parameter name to {"type", "description"}, or NULL. Name it as
 * CAPABILITIES.md says when the vocabulary has it (chassis.move, imu.read);
 * otherwise "<namespace>.<verb>", [A-Za-z0-9._-]{1,64}.
 * `run` returns a result object the SDK takes over: gadget_command_ok() or
 * gadget_command_error(). It runs on the link task and must not block: start
 * long work and return, and offer a status command for its progress.
 */
typedef struct {
    const char *name;
    const char *description;    /* what the agent reads; put ranges and units here */
    const char *required;
    const char *optional;
    cJSON *(*run)(const cJSON *params);
} gadget_command_t;

/* An event the board sends with gadget_send_event(), described for the agent. Kept with the
 * board for its product manifest; link.register no longer carries it. */
typedef struct {
    const char *name;           /* [a-z_]{1,32}, CAPABILITIES.md names first: "picked_up" */
    const char *description;    /* what it means on this device, at most 200 characters */
} gadget_event_t;

#define GADGET_INSTRUCTIONS_MAX 1500    /* the gateway refuses longer */
#define GADGET_EVENTS_MAX 16

/* ---- Hardware the SDK drives through the board ---- */

/* How the on-chip codec is wired. The SDK owns capture, playback, AEC and the voice loop. */
typedef struct {
    bool speaker;               /* false: no speaker.* commands, replies are not spoken */
    bool microphone;            /* false: no push-to-talk */
    int mic_channel;            /* ADC channel carrying the mic: 0 left, 1 right */
    int mic_gain_db;            /* analog gain */
    int speaker_gain_db;
    /* Turns the speaker amplifier on or off; NULL: always on. */
    void (*set_amp)(bool on);
} gadget_audio_t;

/* Battery and supply, read by the SDK every second for device.health and power policy. */
typedef struct {
    bool has_battery;
    int battery_mv;
    int battery_pct;            /* 0..100, the board's own curve */
    bool charging;
    bool usb_power;
} gadget_power_t;

/* Button edges reported by poll_buttons(). The SDK turns them into clicks,
 * multi-clicks and holds, and gives them their standard meanings. */
#define GADGET_BTN_MAIN_PRESS    (1u << 0)
#define GADGET_BTN_MAIN_RELEASE  (1u << 1)
#define GADGET_BTN_AUX_PRESS     (1u << 2)
#define GADGET_BTN_AUX_RELEASE   (1u << 3)

/* States the SDK shows; a board with a status light renders them its own way. */
typedef enum {
    GADGET_STATUS_BOOTING,
    GADGET_STATUS_UNPAIRED,     /* advertising for the Impo app */
    GADGET_STATUS_CONNECTING,
    GADGET_STATUS_IDLE,         /* online */
    GADGET_STATUS_LISTENING,    /* push-to-talk held */
    GADGET_STATUS_THINKING,
    GADGET_STATUS_SPEAKING,
    GADGET_STATUS_OFFLINE,
    GADGET_STATUS_LOW_BATTERY,
    GADGET_STATUS_CHARGING,
    GADGET_STATUS_UPDATING,     /* OTA */
} gadget_status_t;

typedef struct {
    /* Identity, sent in link.register and BLE setup. */
    const char *name;           /* default display name: "ARCS Robot Dog" */
    const char *model_id;       /* "arcs-mini-v2.1-dog" */
    const char *ble_prefix;     /* BLE name before the MAC suffix: "ImpoGadget" */
    /* The product created for this gadget on the Impo developer platform, sent in
     * link.register as product_id: the agent then gets that product's manifest (what the
     * gadget is for, its commands and events). 2 to 40 of a-z, 0-9 and -; NULL or "" when
     * not set up yet. Official units with a unit key attest instead (none on ARCS yet). */
    const char *product_id;

    /* Pins, buses and the board's own parts. Runs first, before the SDK's drivers. */
    int (*init)(void);

    gadget_audio_t audio;

    /* Called every 10 ms from the input task; returns GADGET_BTN_* edges. The first call
     * reports MAIN_PRESS if the key is already held (the power-on press). NULL: no buttons. */
    unsigned (*poll_buttons)(void);

    /* NULL: mains powered, no battery. */
    int (*read_power)(gadget_power_t *out);
    /* Cuts power (releases the latch); returns only on failure. NULL: cannot. */
    int (*power_off)(void);

    /* Shows an SDK state; called on every change. A board command may take the light over
     * (chassis.set_light); show_animation then hands it back to the status. NULL: no status light. */
    void (*show_status)(gadget_status_t status);
    void (*show_animation)(void);

    /* Called when the device goes to standby or wakes; park actuators, pause sensors. NULL: nothing. */
    void (*standby)(bool enter);

    /* Commands only this board has; may be left out. */
    const gadget_command_t *commands;
    int command_count;

    /* What else the board has, for capabilities.features (CAPABILITIES.md):
     * "motion_sensor,touch_pads,locomotion,lights", or NULL. */
    const char *features;

    /* Events this board sends, each with what it means on this device. */
    const gadget_event_t *events;
    int event_count;

    /* The maker's words to the agent that owns the gadget: what it is for and
     * what to do when its events arrive, plain language, at most
     * GADGET_INSTRUCTIONS_MAX characters. NULL: none. Kept for the product manifest;
     * link.register no longer carries it. */
    const char *instructions;
} gadget_board_t;

/* ---- What the SDK gives a board ---- */

/* The board selected at build time (-DBOARD=...). */
const gadget_board_t *gadget_board_get(void);

/* Sends a board event to the agent; data may be NULL. Debounced 3 s per name by the gateway. */
int gadget_send_event(const char *name, cJSON *data);

cJSON *gadget_command_ok(void);                 /* {"ok": true} */
cJSON *gadget_command_ok_payload(cJSON *payload);
cJSON *gadget_command_error(const char *message);

#ifdef __cplusplus
}
#endif
