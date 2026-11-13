// SPDX-License-Identifier: MIT
// A USB keyboard and mouse on the chip's own USB port, for keyboard.type and
// mouse.move.
//
// The S3's USB pins have one PHY, used either by the USB-Serial-JTAG (the
// console and flashing) or by the USB OTG controller. The port stays the
// console until the first keyboard.type or mouse.move. Then TinyUSB takes the
// PHY: the computer sees a keyboard and a mouse, and the serial port goes
// away. After CONFIG_HOMEHUB_USB_HID_RELEASE_S seconds without input the PHY
// goes back to the USB-Serial-JTAG, so the console and esptool work again,
// and the next command plugs the keyboard in again (about a second).
//
// Key presses follow the US layout. The computer turns them into characters
// with its own input source, so an input method such as Pinyin catches them.

#include "usb_hid.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"

#if !defined(CONFIG_TINYUSB_HID_COUNT) || CONFIG_TINYUSB_HID_COUNT < 1
#error "CONFIG_HOMEHUB_USB_HID needs CONFIG_TINYUSB_HID_COUNT=1 in the board's overlay"
#endif

static const char *TAG = "link.usb_hid";

#define USB_HID_TEXT_MAX 500     // characters per keyboard.type, about 10 s
#define USB_HID_MOVE_MAX 3000    // units per axis per mouse.move
#define USB_HID_MOVE_STEP 10     // units per mouse report, for a smooth glide
#define USB_HID_KEY_MS 8         // each press and release is held this long
#define USB_HID_MOUNT_MS 5000    // how long the computer has to take the keyboard
#define USB_HID_SETTLE_MS 500    // after it does, before the first report

// ---- Text and moves to HID reports (host-tested) ----------------------------

#define USB_HID_MOD_LEFT_SHIFT 0x02

// The key that types `ch` on a US keyboard: 1 with *modifier and *keycode
// set, 0 for a character to skip ('\r', so "\r\n" is one Enter), -1 for a
// character a US keyboard can't type.
static int usb_hid_key_for_char(char ch, uint8_t *modifier, uint8_t *keycode) {
    static const char shifted_digits[] = ")!@#$%^&*(";   // Shift + 0..9
    static const struct {
        char plain, shifted;
        uint8_t key;
    } punctuation[] = {
        {'-', '_', 0x2D}, {'=', '+', 0x2E}, {'[', '{', 0x2F}, {']', '}', 0x30},
        {'\\', '|', 0x31}, {';', ':', 0x33}, {'\'', '"', 0x34}, {'`', '~', 0x35},
        {',', '<', 0x36}, {'.', '>', 0x37}, {'/', '?', 0x38},
    };
    *modifier = 0;
    *keycode = 0;
    if (ch >= 'a' && ch <= 'z') {
        *keycode = (uint8_t)(0x04 + (ch - 'a'));
        return 1;
    }
    if (ch >= 'A' && ch <= 'Z') {
        *modifier = USB_HID_MOD_LEFT_SHIFT;
        *keycode = (uint8_t)(0x04 + (ch - 'A'));
        return 1;
    }
    if (ch >= '1' && ch <= '9') {
        *keycode = (uint8_t)(0x1E + (ch - '1'));
        return 1;
    }
    switch (ch) {
        case '0': *keycode = 0x27; return 1;
        case '\n': *keycode = 0x28; return 1;   // Enter
        case '\t': *keycode = 0x2B; return 1;   // Tab
        case ' ': *keycode = 0x2C; return 1;
        case '\r': return 0;
        default: break;
    }
    for (int digit = 0; digit < 10; digit++) {
        if (ch == shifted_digits[digit]) {
            *modifier = USB_HID_MOD_LEFT_SHIFT;
            *keycode = (uint8_t)(digit == 0 ? 0x27 : 0x1E + digit - 1);
            return 1;
        }
    }
    for (size_t i = 0; i < sizeof(punctuation) / sizeof(punctuation[0]); i++) {
        if (ch == punctuation[i].plain || ch == punctuation[i].shifted) {
            if (ch == punctuation[i].shifted) *modifier = USB_HID_MOD_LEFT_SHIFT;
            *keycode = punctuation[i].key;
            return 1;
        }
    }
    return -1;
}

// The index of the first character in `text` that a US keyboard can't type,
// or -1 when it can type them all.
static int usb_hid_untypable_at(const char *text) {
    for (int i = 0; text[i]; i++) {
        uint8_t modifier, keycode;
        if (usb_hid_key_for_char(text[i], &modifier, &keycode) < 0) return i;
    }
    return -1;
}

// How many mouse reports a move by (dx, dy) takes, at most `max_step` units
// per axis each. 0 for no move.
static int usb_hid_move_steps(int dx, int dy, int max_step) {
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    int longest = ax > ay ? ax : ay;
    return (longest + max_step - 1) / max_step;
}

// Report `step` (1..steps) of a straight move by `total` along one axis. The
// reports add up to `total` exactly, and none is larger than the step size.
static int usb_hid_move_delta(int total, int step, int steps) {
    return total * step / steps - total * (step - 1) / steps;
}

// ---- USB device -------------------------------------------------------------

enum { REPORT_ID_KEYBOARD = 1, REPORT_ID_MOUSE = 2 };

static const uint8_t s_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
};

#define USB_HID_CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_config_desc[] = {
    // Configuration 1, one interface, no string, remote wakeup, 100 mA.
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, USB_HID_CONFIG_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    // Interface 0, string 4, no boot protocol, endpoint 0x81, 16 bytes, polled every 5 ms.
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, sizeof(s_report_desc), 0x81, 16, 5),
};

static char s_serial[13];   // the MAC, set before the first attach
static const char *s_strings[] = {
    (const char[]){0x09, 0x04},   // English (0x0409)
    "Impo",
    "Impo Gadget keyboard and mouse",
    s_serial,
    "Impo keyboard and mouse",
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return s_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

static bool s_attached;
// The PHY handed back to the USB-Serial-JTAG after a release. At boot the
// console owns the PHY without a handle, so this is NULL until then.
static usb_phy_handle_t s_console_phy;

// Back to the serial console.
static void usb_hid_release(void) {
    if (!s_attached) return;
    esp_err_t err = tinyusb_driver_uninstall();
    s_attached = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TinyUSB uninstall failed: %s", esp_err_to_name(err));
    }
    usb_phy_config_t phy = {
        .controller = USB_PHY_CTRL_SERIAL_JTAG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_PHY_MODE_DEFAULT,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,
    };
    err = usb_new_phy(&phy, &s_console_phy);
    if (err != ESP_OK) {
        s_console_phy = NULL;
        ESP_LOGW(TAG, "USB-Serial-JTAG didn't get the USB port back: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "USB port is the serial console again");
}

// Makes the USB port a keyboard and mouse that a computer has taken. NULL on
// success, otherwise why not (the port is the console again).
static const char *usb_hid_attach(void) {
    if (s_attached && tud_mounted()) return NULL;
    if (s_attached) usb_hid_release();   // unplugged meanwhile: start afresh

    if (!s_serial[0]) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    ESP_LOGI(TAG, "USB port becomes a keyboard and mouse; the serial console stops until %d s "
             "without input", CONFIG_HOMEHUB_USB_HID_RELEASE_S);
    vTaskDelay(pdMS_TO_TICKS(50));   // let that line out before the port goes
    if (s_console_phy) {
        usb_del_phy(s_console_phy);
        s_console_phy = NULL;
    }

    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    config.descriptor.string = s_strings;
    config.descriptor.string_count = sizeof(s_strings) / sizeof(s_strings[0]);
    config.descriptor.full_speed_config = s_config_desc;
    esp_err_t err = tinyusb_driver_install(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TinyUSB install failed: %s", esp_err_to_name(err));
        usb_phy_config_t phy = {
            .controller = USB_PHY_CTRL_SERIAL_JTAG,
            .target = USB_PHY_TARGET_INT,
            .otg_mode = USB_PHY_MODE_DEFAULT,
            .otg_speed = USB_PHY_SPEED_UNDEFINED,
        };
        if (usb_new_phy(&phy, &s_console_phy) != ESP_OK) s_console_phy = NULL;
        return "the USB port couldn't become a keyboard";
    }
    s_attached = true;

    int64_t deadline = esp_timer_get_time() + USB_HID_MOUNT_MS * 1000LL;
    while (!tud_mounted() && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!tud_mounted()) {
        usb_hid_release();
        return "no computer took the keyboard: plug the board's USB-C port into a computer, "
               "not a charger, and try again";
    }
    vTaskDelay(pdMS_TO_TICKS(USB_HID_SETTLE_MS));
    ESP_LOGI(TAG, "keyboard and mouse ready");
    return NULL;
}

// Waits until the next report can go out, waking a sleeping computer first.
static bool usb_hid_wait_ready(void) {
    int64_t deadline = esp_timer_get_time() + 1000 * 1000LL;
    while (esp_timer_get_time() < deadline) {
        if (tud_suspended()) {
            tud_remote_wakeup();
        } else if (!tud_mounted()) {
            return false;
        } else if (tud_hid_ready()) {
            return true;
        }
        vTaskDelay(1);
    }
    return false;
}

static bool usb_hid_press(uint8_t modifier, uint8_t keycode) {
    uint8_t keys[6] = {keycode};
    if (!usb_hid_wait_ready() || !tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, keys)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(USB_HID_KEY_MS));
    if (!usb_hid_wait_ready() || !tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, NULL)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(USB_HID_KEY_MS));
    return true;
}

// ---- Commands ---------------------------------------------------------------

typedef enum { USB_HID_TYPE, USB_HID_MOVE } usb_hid_kind_t;

typedef struct {
    usb_hid_kind_t kind;
    char *text;
    int dx, dy;
    noise_ctrl_session_generation_t session_generation;
    char request_id[64];
} usb_hid_job_t;

static QueueHandle_t s_jobs;

static cJSON *usb_hid_error(const char *code, const char *message) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_AddObjectToObject(result, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return result;
}

static cJSON *usb_hid_run(const usb_hid_job_t *job) {
    const char *failure = usb_hid_attach();
    if (failure) return usb_hid_error("no_computer", failure);

    cJSON *result = cJSON_CreateObject();
    if (job->kind == USB_HID_TYPE) {
        int typed = 0;
        for (const char *p = job->text; *p; p++) {
            uint8_t modifier, keycode;
            if (usb_hid_key_for_char(*p, &modifier, &keycode) <= 0) continue;
            if (!usb_hid_press(modifier, keycode)) {
                cJSON_Delete(result);
                char message[96];
                snprintf(message, sizeof(message),
                         "the computer stopped taking key presses after %d characters", typed);
                return usb_hid_error("usb_lost", message);
            }
            typed++;
        }
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddNumberToObject(cJSON_AddObjectToObject(result, "payload"), "typed", typed);
        ESP_LOGI(TAG, "typed %d characters", typed);
    } else {
        int steps = usb_hid_move_steps(job->dx, job->dy, USB_HID_MOVE_STEP);
        for (int step = 1; step <= steps; step++) {
            int8_t x = (int8_t)usb_hid_move_delta(job->dx, step, steps);
            int8_t y = (int8_t)usb_hid_move_delta(job->dy, step, steps);
            if (!usb_hid_wait_ready() || !tud_hid_mouse_report(REPORT_ID_MOUSE, 0, x, y, 0, 0)) {
                cJSON_Delete(result);
                return usb_hid_error("usb_lost", "the computer stopped taking mouse moves");
            }
            vTaskDelay(pdMS_TO_TICKS(USB_HID_KEY_MS));
        }
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON *payload = cJSON_AddObjectToObject(result, "payload");
        cJSON_AddNumberToObject(payload, "dx", job->dx);
        cJSON_AddNumberToObject(payload, "dy", job->dy);
        ESP_LOGI(TAG, "moved the pointer by %d, %d", job->dx, job->dy);
    }
    return result;
}

static void usb_hid_task(void *arg) {
    (void)arg;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (s_attached && CONFIG_HOMEHUB_USB_HID_RELEASE_S > 0) {
            wait = pdMS_TO_TICKS(CONFIG_HOMEHUB_USB_HID_RELEASE_S * 1000);
        }
        usb_hid_job_t *job = NULL;
        if (xQueueReceive(s_jobs, &job, wait) != pdTRUE) {
            usb_hid_release();
            continue;
        }
        cJSON *result = usb_hid_run(job);
        noise_ctrl_send_command_result(job->session_generation, job->request_id, result);
        free(job->text);
        free(job);
    }
}

// Starts the queue and the task on the first command. Only the Link session's
// task calls this, so there's no race.
static bool usb_hid_start(void) {
    if (s_jobs) return true;
    QueueHandle_t jobs = xQueueCreate(4, sizeof(usb_hid_job_t *));
    if (!jobs) return false;
    if (xTaskCreate(usb_hid_task, "usb_hid", 4096, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(jobs);
        return false;
    }
    s_jobs = jobs;
    return true;
}

static bool usb_hid_int_param(const cJSON *params, const char *key, int *value) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(params, key);
    if (!cJSON_IsNumber(item)) return false;
    double v = item->valuedouble;
    if (v < -USB_HID_MOVE_MAX || v > USB_HID_MOVE_MAX || v != (double)(int)v) return false;
    *value = (int)v;
    return true;
}

cJSON *usb_hid_command(const char *command, const cJSON *params,
                       const char *request_id,
                       noise_ctrl_session_generation_t session_generation) {
    bool type = strcmp(command, "keyboard.type") == 0;
    bool move = strcmp(command, "mouse.move") == 0;
    if (!type && !move) return NULL;
    if (!cJSON_IsObject(params)) return usb_hid_error("invalid_param", "params must be an object");

    usb_hid_job_t *job = calloc(1, sizeof(*job));
    if (!job) return usb_hid_error("out_of_memory", "no memory for the input");
    if (type) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(params, "text");
        size_t length = cJSON_IsString(text) && text->valuestring ? strlen(text->valuestring) : 0;
        if (length == 0 || length > USB_HID_TEXT_MAX) {
            free(job);
            return usb_hid_error("invalid_param", "text must be 1 to 500 characters");
        }
        int bad = usb_hid_untypable_at(text->valuestring);
        if (bad >= 0) {
            free(job);
            char message[128];
            snprintf(message, sizeof(message),
                     "character %d of text can't be typed on a US keyboard: send printable "
                     "ASCII, \\n for Enter and \\t for Tab", bad);
            return usb_hid_error("invalid_param", message);
        }
        job->kind = USB_HID_TYPE;
        job->text = strdup(text->valuestring);
        if (!job->text) {
            free(job);
            return usb_hid_error("out_of_memory", "no memory for the text");
        }
    } else {
        job->kind = USB_HID_MOVE;
        if (!usb_hid_int_param(params, "dx", &job->dx) || !usb_hid_int_param(params, "dy", &job->dy)) {
            free(job);
            return usb_hid_error("invalid_param", "dx and dy must be whole numbers from -3000 to 3000");
        }
    }
    job->session_generation = session_generation;
    snprintf(job->request_id, sizeof(job->request_id), "%s", request_id ? request_id : "");

    if (!usb_hid_start()) {
        free(job->text);
        free(job);
        return usb_hid_error("out_of_memory", "couldn't start the keyboard task");
    }
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        free(job->text);
        free(job);
        return usb_hid_error("busy", "still sending earlier input; try again when it has finished");
    }
    cJSON *async = cJSON_CreateObject();
    cJSON_AddBoolToObject(async, "_async", true);
    return async;
}
