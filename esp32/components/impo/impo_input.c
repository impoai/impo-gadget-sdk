/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

#include "impo_input.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

#include "impo_battery.h"
#include "impo_ble.h"
#include "impo_board.h"
#include "impo_chat.h"
#include "impo_console.h"
#include "impo_link.h"
#include "impo_mem.h"
#include "impo_menu.h"
#include "impo_settings.h"
#include "impo_state.h"
#include "impo_ui.h"
#include "impo_voice.h"
#include "impo_wifi.h"
#if CONFIG_IMPO_BOARD_ESPRESSIF_SPARKBOT
#include "boards/sparkbot_console.h"
#endif
#if CONFIG_IMPO_WATCHER_CAMERA
#include "boards/watcher_camera.h"
#endif

static const char *TAG = "impo_input";

#define POLL_MS 10
#define REST_POLL_MS 50        /* screen off and paused: still quick to wake */
#define REST_WAIT_MS 1000      /* paused, buttons that interrupt: the rest waits this long */
#define NAP_WAIT_MS 10000      /* ... and Wi-Fi napping */
#define POWER_MS 2000          /* refresh battery */
#define REST_POWER_MS 10000    /* ... while paused */
#define WIFI_NAP_MS (2 * 60 * 1000)   /* low power this long: Wi-Fi off until it ends */
#define DOUBLE_TICKS 35        /* 350 ms: a second aux press within this toggles phone setup */

#define GOODBYE_MS 1500        /* let the goodbye animation play */
#define HINT_TICKS 60          /* 0.6 s: warn that holding powers off */
#define LONG_TICKS 150         /* 1.5 s: power off */
#define SLEEP_CHECK_MS 100

#define SERIAL_RX 1024         /* the driver drops what doesn't fit, so a console line must */
#define SERIAL_LINE 1024
#define CHAT_MAX (192 * 1024)  /* a typed message, assembled from "chat+=" lines */

static QueueHandle_t s_queue;
static TaskHandle_t s_input;
static bool s_talk_down;
static bool s_cpu_low;      /* display stopped and the CPU allowed to sleep */
static volatile bool s_power_off_requested;
static volatile bool s_nap_now;   /* ">nap": asleep, as if on battery, nap without waiting WIFI_NAP_MS */

static void post(impo_ptt_t type, bool wake)
{
    impo_input_event_t ev = { .type = type, .wake = wake };
    ESP_LOGI(TAG, "PTT %s%s", type == IMPO_PTT_DOWN ? "down" : "up", wake ? " (waking)" : "");
    xQueueSend(s_queue, &ev, 0);
}

static bool update_power(void);

static void power_off(void)
{
    ESP_LOGI(TAG, "shutting down");
    impo_state_set_asleep(false);
    update_power();
    impo_state_set_progress(0);
    impo_state_set_level(0);
    impo_state_set_mode(IMPO_MODE_OFF);
    impo_state_set_caption("GOODBYE!");
    vTaskDelay(pdMS_TO_TICKS(GOODBYE_MS));
    esp_err_t err = impo_board->power_off();
    /* Only reached if the board couldn't power off. */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGE(TAG, "power-off failed (%s)", esp_err_to_name(err));
    impo_state_set_mode(IMPO_MODE_IDLE);
    impo_state_set_caption("COULDN'T POWER OFF");
}

static void set_asleep(bool asleep, const char *why)
{
    if (asleep != impo_state_asleep()) {
        ESP_LOGI(TAG, "%s (%s)", asleep ? "sleeping" : "waking", why);
        impo_state_set_asleep(asleep);
        if (s_input) {
            xTaskNotifyGive(s_input);   /* out of wait_buttons() */
        }
        if (!asleep && !impo_wifi_connected()) {
            impo_wifi_apply();   /* the screen says reconnecting: don't sit out the backoff */
        }
    }
}

static void toggle_phone_setup(void)
{
    bool on = !impo_settings_ble_on();
    ESP_LOGI(TAG, "phone setup %s", on ? "on" : "off");
    impo_settings_set_ble_on(on);
    impo_ble_status_t b;
    impo_ble_status(&b);
    if (on && b.name[0]) {
        impo_state_set_caption("PHONE SETUP: %s", b.name);
    } else {
        impo_state_set_caption("PHONE SETUP %s", on ? "ON" : "OFF");
    }
}

/*
 * Aux button: short press sleeps, a 1.5 s hold powers off, any press wakes.
 * Two quick presses toggle BLE phone setup, so the sleep waits a moment to
 * see whether a second press follows.
 */
static void aux_button(bool pressed, bool edge)
{
    static int held;
    static bool swallow;
    static bool hinted;
    static int sleep_in;    /* ticks until a pending single press sleeps */
    static char saved_caption[64];

    if (sleep_in && --sleep_in == 0) {
        set_asleep(true, impo_board->aux_button);
    }
    if (edge && pressed) {
        held = 0;
        hinted = false;
        swallow = impo_state_asleep();
        if (swallow) {
            set_asleep(false, impo_board->aux_button);
        } else if (sleep_in) {
            sleep_in = 0;
            swallow = true;
            toggle_phone_setup();
        }
        return;
    }
    if (pressed && !swallow) {
        held++;
        if (held == HINT_TICKS) {
            uint32_t v = UINT32_MAX;
            impo_state_caption(saved_caption, sizeof(saved_caption), &v);
            impo_state_set_caption("HOLD TO POWER OFF");
            hinted = true;
        } else if (held == LONG_TICKS) {
            swallow = true;
            power_off();
        }
        return;
    }
    if (edge && !pressed && !swallow) {
        if (hinted) {
            impo_state_set_caption("%s", saved_caption);   /* let go early: cancel */
        } else {
            sleep_in = DOUBLE_TICKS;
        }
    }
}

/* No touch: the aux button opens the menu and steps down it; any press wakes. */
static void menu_button(bool pressed, bool edge)
{
    if (!edge || !pressed) {
        return;
    }
    if (impo_state_asleep()) {
        set_asleep(false, impo_board->aux_button);
    } else if (!s_talk_down) {
        impo_state_poke();
        impo_menu_key(IMPO_MENU_DOWN);
    }
}

static void aux_key(bool pressed, bool edge)
{
    if (impo_board->touch) {
        aux_button(pressed, edge);
    } else {
        menu_button(pressed, edge);
    }
}

/* Talk button: push-to-talk, or Select while the menu is open. Asleep, the
 * press wakes and is posted as a waking one: impo_voice records only if it's
 * still held once awake. */
static void talk_button(unsigned ev)
{
    bool talk_down = s_talk_down;
    static bool swallow;
#if CONFIG_IMPO_WATCHER_CAMERA
    static TickType_t last_release;
    if ((ev & IMPO_BTN_TALK_PRESS) && !impo_state_asleep()
        && !impo_menu_is_open() && last_release
        && xTaskGetTickCount() - last_release <= pdMS_TO_TICKS(350)) {
        ESP_LOGI(TAG, "wheel double-click: camera preview/shutter");
        last_release = 0;
        watcher_camera_preview_toggle();
        swallow = true;
        return;
    }
#endif

    /* A quick tap can latch press and release in the same poll, and a release
     * can land just before the next press; keep them ordered. */
    bool released = ev & IMPO_BTN_TALK_RELEASE;
#if CONFIG_IMPO_WATCHER_CAMERA
    bool saw_release = released;
#endif
    if ((talk_down || swallow) && released) {
        if (talk_down) {
            post(IMPO_PTT_UP, false);
        }
        talk_down = swallow = false;
        released = false;
    }
    if (!talk_down && !swallow && (ev & IMPO_BTN_TALK_PRESS)) {
        if (impo_link_talk_press()) {
            /* Confirmed a Impo app pairing (Link's setup button). */
            impo_state_poke();
            swallow = true;
        } else if (impo_state_asleep()) {
            set_asleep(false, impo_board->talk_button);
            post(IMPO_PTT_DOWN, true);
            talk_down = true;
        } else if (impo_menu_is_open()) {
            impo_state_poke();
            if (!impo_board->keyboard) impo_menu_key(IMPO_MENU_SELECT);
            swallow = true;
        } else {
            post(IMPO_PTT_DOWN, false);
            talk_down = true;
        }
    }
    if ((talk_down || swallow) && released) {
        if (talk_down) {
            post(IMPO_PTT_UP, false);
        }
        talk_down = swallow = false;
    }
#if CONFIG_IMPO_WATCHER_CAMERA
    if (saw_release && !swallow) {
        last_release = xTaskGetTickCount();
    }
#endif
    s_talk_down = talk_down;
}

/* Keyboard menus don't repurpose Space/GO as Select. Navigation wakes the
 * screen without accidentally changing a setting; Enter can confirm pairing. */
static void keyboard_buttons(unsigned ev)
{
    const unsigned mask = IMPO_BTN_UP | IMPO_BTN_DOWN | IMPO_BTN_LEFT |
                          IMPO_BTN_RIGHT | IMPO_BTN_ENTER | IMPO_BTN_ESCAPE;
    if (!(ev & mask) || s_talk_down) return;
    if ((ev & IMPO_BTN_ENTER) && impo_link_talk_press()) {
        impo_state_poke();
        return;
    }
    if (impo_state_asleep()) {
        set_asleep(false, "keyboard");
        return;
    }
    impo_state_poke();
    if (ev & IMPO_BTN_ESCAPE) impo_menu_key(IMPO_MENU_BACK);
    else if (ev & IMPO_BTN_UP) impo_menu_key(IMPO_MENU_UP);
    else if (ev & IMPO_BTN_DOWN) impo_menu_key(IMPO_MENU_DOWN);
    else if (ev & IMPO_BTN_LEFT) impo_menu_key(IMPO_MENU_LEFT);
    else if (ev & IMPO_BTN_RIGHT) impo_menu_key(IMPO_MENU_RIGHT);
    else if (ev & IMPO_BTN_ENTER) impo_menu_key(IMPO_MENU_SELECT);
}

/* A pairing prompt wakes the screen and keeps it on; otherwise idle sleeps. */
static void check_sleep(void)
{
    impo_ble_status_t ble;
    impo_ble_status(&ble);
    bool prompt = ble.passkey || impo_link_state() == IMPO_LINK_CONFIRM;
    if (prompt) {
        set_asleep(false, "pairing");
        return;
    }
    int after = impo_settings_sleep_s();
    float mode_t;
    if (after && !impo_state_asleep() && impo_state_mode(&mode_t) == IMPO_MODE_IDLE
        && impo_state_idle_secs() > after) {
        set_asleep(true, "auto-sleep");
    }
}

static void set_cpu_low(bool low)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = low ? CONFIG_XTAL_FREQ : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = low,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power management: %s", esp_err_to_name(err));
    }
#else
    (void)low;
#endif
}

/*
 * On battery with the screen dark and voice resting: stop the display and let
 * the CPU drop to the crystal clock and light-sleep between polls, or until a
 * button interrupts (wait_buttons). Returns true while the display is stopped.
 * With a USB host attached (">nap" on the bench) the CPU stays at full speed:
 * at the crystal clock a long line of serial output can stall the USB console
 * until the host reopens the port.
 */
static bool update_power(void)
{
    static bool paused;
    bool pause = impo_board->display_pause && impo_state_on_battery() && impo_state_asleep()
                 && impo_ui_dark() && impo_voice_resting();
    bool want_low = pause && !impo_console_host();
    if (pause == paused && want_low == s_cpu_low) {
        return paused;
    }
    if (pause && !paused) {
        impo_board->display_pause(true);
    }
    if (want_low != s_cpu_low) {
        set_cpu_low(want_low);
    }
    if (!pause && paused) {
        impo_board->display_pause(false);
    }
    paused = pause;
    s_cpu_low = want_low;
    ESP_LOGI(TAG, "%s", s_cpu_low ? "low power: display paused"
                        : paused  ? "display paused (USB host: CPU at full speed)"
                                  : "full power");
    return paused;
}

/*
 * Low power for WIFI_NAP_MS: Wi-Fi off, since keeping it associated costs
 * more than the rest of the chip. It rejoins when the screen wakes or USB
 * power arrives; meanwhile nothing reaches the device over the network.
 * Not while a voice note recorded offline waits to go (for a while). Returns
 * true while napping.
 */
static bool update_wifi_nap(TickType_t now, bool paused)
{
    static TickType_t low_since;
    static bool napping;
    if (!s_cpu_low) {
        low_since = now;
    }
    if (!impo_state_asleep() && s_nap_now) {
        s_nap_now = false;
        impo_state_set_as_if_battery(false);
    }
    bool nap = paused && !impo_voice_notes_waiting()
               && (s_nap_now || (s_cpu_low && now - low_since >= pdMS_TO_TICKS(WIFI_NAP_MS)));
    if (nap != napping) {
        napping = nap;
        ESP_LOGI(TAG, "Wi-Fi %s", nap ? "napping" : "waking");
        impo_wifi_nap(nap);
    }
    return napping;
}

static void input_task(void *arg)
{
    (void)arg;
    bool aux_down = false;
    bool paused = false;
    TickType_t checked = xTaskGetTickCount() - pdMS_TO_TICKS(SLEEP_CHECK_MS);
    TickType_t powered = xTaskGetTickCount() - pdMS_TO_TICKS(POWER_MS);

    for (;;) {
        unsigned ev = impo_board->poll_buttons();
        if (ev & (IMPO_BTN_TALK_PRESS | IMPO_BTN_TALK_RELEASE)) {
            ESP_LOGI(TAG, "talk key:%s%s", ev & IMPO_BTN_TALK_PRESS ? " press" : "",
                     ev & IMPO_BTN_TALK_RELEASE ? " release" : "");
            talk_button(ev);
        }
        keyboard_buttons(ev);
        /* A latched key (the 1.75's PMU) can report press and release in the
         * same poll, and a release can land just before the next press; keep
         * them ordered, as talk_button does. */
        bool aux_press = ev & IMPO_BTN_AUX_PRESS;
        bool aux_release = ev & IMPO_BTN_AUX_RELEASE;
        bool aux_edge = false;
        if (aux_down && aux_release) {
            aux_down = false;
            aux_release = false;
            aux_edge = true;
            aux_key(false, true);
        }
        if (!aux_down && aux_press) {
            aux_down = aux_edge = true;
            aux_key(true, true);
            if (aux_release) {
                aux_down = false;
                aux_key(false, true);
            }
        }
        if (!aux_edge) {
            aux_key(aux_down, false);
        }

        if (s_power_off_requested) {
            s_power_off_requested = false;
            power_off();
        }

        TickType_t now = xTaskGetTickCount();
        if (now - checked >= pdMS_TO_TICKS(SLEEP_CHECK_MS)) {
            checked = now;
            check_sleep();
        }

        if (now - powered >= pdMS_TO_TICKS(paused ? REST_POWER_MS : POWER_MS)) {
            powered = now;
            impo_power_t p = { .battery_pct = -1 };
            if (impo_board->read_power && impo_board->read_power(&p) == ESP_OK) {
                impo_state_set_power(&p);
                impo_battery_note_power(&p, impo_state_on_battery());
            }
        }

        paused = update_power();
        bool napping = update_wifi_nap(now, paused);
        impo_battery_note_state(impo_state_asleep(), s_cpu_low);
        /* Paused, anything but a button (a pairing prompt, an image) is
         * noticed within REST_WAIT_MS. Napping, nothing comes over the
         * network; USB power arriving is noticed within NAP_WAIT_MS. */
        if (paused && impo_board->wait_buttons) {
            impo_board->wait_buttons(napping ? NAP_WAIT_MS : REST_WAIT_MS);
        } else {
            vTaskDelay(pdMS_TO_TICKS(paused ? REST_POLL_MS : POLL_MS));
        }
    }
}

/* Reads the rest of a line into buf; false if it was too long (the rest is dropped). */
static bool read_line(char *buf, size_t cap)
{
    size_t len = 0;
    bool whole = true;
    uint8_t c;
    while (impo_console_getc(&c) && c != '\n') {
        if (c == '\r') {
            continue;
        }
        if (len < cap - 1) {
            buf[len++] = (char)c;
        } else {
            whole = false;
        }
    }
    buf[len] = '\0';
    return whole;
}

#if CONFIG_IMPO_HATCH
#define CHAT_OVER_SERIAL "true"

/*
 * "chat+=TEXT" adds a piece of a message and "chat=TEXT" adds the last piece
 * and sends it (TEXT escaped: \n \r \t \\). Every line is acknowledged, and the
 * host waits for that before the next: the driver drops what it has no room for.
 */
static char *s_chat;        /* the message so far */
static size_t s_chat_len;

static void chat_line(char *piece, bool last, bool whole)
{
    size_t n = impo_hatch_unescape(piece);
    if (!s_chat) {
        s_chat = heap_caps_malloc(CHAT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_chat_len = 0;
    }
    const char *err = !whole ? "LINE TOO LONG" : !s_chat ? "OUT OF MEMORY" : s_chat_len + n >= CHAT_MAX ? "TOO LONG" : NULL;
    if (err) {
        free(s_chat);
        s_chat = NULL;
        impo_hatch_console("error", err, NULL);
        return;
    }
    memcpy(s_chat + s_chat_len, piece, n);
    s_chat_len += n;
    s_chat[s_chat_len] = '\0';
    impo_hatch_console("ack", NULL, "\"bytes\":%u", (unsigned)s_chat_len);
    if (last) {
        impo_hatch_text_turn(s_chat);   /* frees it */
        s_chat = NULL;
    }
}

/* Drops a half-sent message and ends a typed turn. */
static void chat_cancel(void)
{
    free(s_chat);
    s_chat = NULL;
    impo_hatch_text_cancel();
}
#else
#define CHAT_OVER_SERIAL "false"   /* no PSRAM: replies come over Link, and only short ones */
#endif

/*
 * Bench: puts the face in a mode ("face=thinking") until the voice path or
 * another "face=" moves it on. "face=happy" goes back to idle with the happy
 * hop, as a finished turn does.
 */
static void set_face(const char *name)
{
    static const char *const modes[IMPO_MODE_COUNT] = {
        [IMPO_MODE_BOOT] = "boot",
        [IMPO_MODE_IDLE] = "idle",
        [IMPO_MODE_LISTENING] = "listening",
        [IMPO_MODE_THINKING] = "thinking",
        [IMPO_MODE_SPEAKING] = "speaking",
        [IMPO_MODE_ERROR] = "error",
        [IMPO_MODE_OFF] = "off",
    };
    impo_state_poke();
    bool happy = !strcmp(name, "happy");
    if (happy) {
        name = "idle";   /* where a finished turn hops to */
    }
    for (int m = 0; m < IMPO_MODE_COUNT; m++) {
        if (!strcmp(name, modes[m])) {
            impo_state_set_mode(IMPO_MODE_IDLE);   /* only idle leaves "off" */
            impo_state_set_mode((impo_mode_t)m);
            if (happy) {
                impo_state_make_happy();
            }
            return;
        }
    }
    printf("@face.error unknown face \"%s\": boot idle listening thinking speaking error off happy\n", name);
    fflush(stdout);
}

/*
 * Console-only commands; false for setup commands. Their buffers are taken
 * per command: without PSRAM, static ones would hold internal RAM for good.
 */
static bool console_command(char *line, bool whole)
{
#if CONFIG_IMPO_BOARD_ESPRESSIF_SPARKBOT
    if (impo_sparkbot_console(line, whole)) {
        return true;
    }
#endif
    if (!strncmp(line, "event=", 6) && whole) {
        /* Bench: a link.event by name, as a board would send it ("event=shaken"). */
        bool sent = impo_link_send_event(line + 6, NULL);
        printf("@event {\"event\":\"%s\",\"sent\":%s}\n", line + 6, sent ? "true" : "false");
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "status")) {
        size_t cap = 1024;   /* long SSID, host and VM names escaped: past 512 */
        char *json = heap_caps_malloc(cap, IMPO_BIG_CAPS);
        if (json) {
            impo_ble_status_json(json, cap);
            printf("@status {\"board\":\"%s\",\"chat\":%s,\"device\":%s}\n", impo_board->name,
                   CHAT_OVER_SERIAL, json);
            fflush(stdout);
            free(json);
        }
        return true;
    }
    bool reset = !strcmp(line, "power.reset");
    if (reset || !strcmp(line, "power")) {
        if (reset) {
            impo_battery_reset();
        }
        const char *saved = impo_battery_saved_json();
        if (saved) {
            printf("@power.saved %s\n", saved);
        }
        size_t cap = 2048;   /* two dozen power locks */
        char *json = heap_caps_malloc(cap, IMPO_BIG_CAPS);
        if (json) {
            impo_battery_json(json, cap);
            printf("@power %s\n", json);
            free(json);
        }
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "nap")) {
        impo_state_set_as_if_battery(true);
        set_asleep(true, "serial");
        s_nap_now = true;
        return true;
    }
    if (!strncmp(line, "face=", 5)) {
        set_face(line + 5);
        return true;
    }
    if (strncmp(line, "chat", 4) != 0) {
        return false;
    }
#if CONFIG_IMPO_HATCH
    if (!strcmp(line, "chat.cancel")) {
        chat_cancel();
        return true;
    }
    bool last = !strncmp(line, "chat=", 5);
    if (last || !strncmp(line, "chat+=", 6)) {
        chat_line(line + (last ? 5 : 6), last, whole);
        return true;
    }
    return false;
#else
    (void)whole;
    impo_hatch_console("error", "THIS BOARD CAN'T CHAT OVER SERIAL", NULL);
    return true;
#endif
}

/*
 * Bench testing over the USB cable: 'd' / 'u' act as the talk button going
 * down / up, so the voice path can be driven without a finger on the button;
 * 'm' plays a built-in MP3 through the reply decoder; 'a' / 's' press the
 * menu's Down / Select; 'p' sends a screenshot; 'z' / 'w' sleep and wake.
 * A line starting with '>' is a setup command, the same "key=value" text as
 * the BLE CMD characteristic, or one of the console's own: "status" prints
 * the device's state, "power" the battery meter (impo_battery.h) and
 * "power.reset" starts it over, "nap" sleeps and leaves Wi-Fi at once (as
 * two minutes asleep on battery would; 'w' rejoins), "face=" shows a face
 * (see set_face), and "chat=" sends a typed message to Hatch (see chat_line
 * and tools/impo/chat.py).
 */
static void serial_task(void *arg)
{
    if (impo_console_install(SERIAL_RX) != ESP_OK) {
        vTaskDelete(NULL);
    }
    for (;;) {
        uint8_t c;
        if (!impo_console_getc(&c)) {
            continue;
        }
        if (c == 'm') {
            impo_voice_request_mp3test();
        } else if (c == 'a' || c == 's') {
            impo_state_poke();
            impo_menu_key(c == 'a' ? IMPO_MENU_DOWN : IMPO_MENU_SELECT);
        } else if (c == 'p') {
            impo_ui_request_snapshot();
        } else if (c == 'z' || c == 'w') {
            set_asleep(c == 'z', "serial");
        } else if (c == '>') {
            char *line = heap_caps_malloc(SERIAL_LINE, IMPO_BIG_CAPS);
            char none[1];   /* no room: the line is read and dropped */
            bool whole = read_line(line ? line : none, line ? SERIAL_LINE : sizeof(none));
            if (line && !console_command(line, whole)) {
                impo_ble_command(line);
            }
            free(line);
        } else if (c == 'd' || c == 'u') {
            impo_state_poke();
            post(c == 'd' ? IMPO_PTT_DOWN : IMPO_PTT_UP, false);
        }
    }
}

esp_err_t impo_input_start(QueueHandle_t queue)
{
    s_queue = queue;
    if (xTaskCreate(input_task, "impo_input", 4096, NULL, 6, &s_input) != pdPASS) {
        return ESP_FAIL;
    }
    /* Bench-test and setup console; the input still works if it can't start. */
    xTaskCreate(serial_task, "impo_serial", 3584, NULL, 5, NULL);
    return ESP_OK;
}

void impo_input_request_power_off(void)
{
    s_power_off_requested = true;
}
