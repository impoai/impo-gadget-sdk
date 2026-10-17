// SPDX-License-Identifier: Apache-2.0
// Original Xteink X4: ESP32-C3, 16 MB flash, no PSRAM, SSD1677 800x480.
// Pin map and SSD1677 register sequences adapted from FreeInk (MIT):
// https://github.com/Free-Ink/freeink-sdk/tree/111fdcc7f0176c3ee38391a160ee296bf492dbd8
// libs/hardware/BoardConfig, InputManager, display/FreeInkDisplay/Ssd1677Driver.
// See ../licenses/FreeInk-MIT.txt. Not for X3, X4 Pro, Classic or UC panels.
#include "xteink_x4.h"
#include "display_snapshot.h"
#include "esp_partition.h"
#include "xteink_x4_pixels.h"
#include "led_status.h"
#include "pixel_font.h"
#include "button.h"
#include "gadget_power.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "display_canvas.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

enum { PIN_SCK = 8, PIN_MOSI = 10, PIN_CS = 21, PIN_DC = 4,
       PIN_RST = 5, PIN_BUSY = 6, PIN_SD_CS = 12, PIN_LATCH = 13, PIN_POWER = 3 };
static const char *TAG = "link.x4";
static spi_device_handle_t s_spi;
static adc_oneshot_unit_handle_t s_adc;
static uint8_t *s_frame;
static SemaphoreHandle_t s_frame_lock;
static portMUX_TYPE s_meta_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready, s_dirty, s_loading, s_canvas_pending, s_canvas_refreshing;
static bool s_image_refresh = true;
static bool s_image_clear, s_image_changed;
static bool s_power_image_busy, s_power_image_ready, s_power_image_stored;
static display_snapshot_t s_power_image;
static const esp_partition_t *s_snapshot_partition;
static enum { VIEW_STATUS, VIEW_TEXT, VIEW_IMAGE, VIEW_CANVAS } s_view;
static led_state_t s_state = LED_STATE_BOOT;
static char s_title[65], s_text[201];
static void request_power_off(void);

static bool snapshot_read(size_t offset, void *data, size_t len) {
    return offset <= s_snapshot_partition->size && len <= s_snapshot_partition->size - offset &&
        esp_partition_read(s_snapshot_partition, offset, data, len) == ESP_OK;
}
static bool snapshot_erase(size_t offset, size_t len) {
    return offset <= s_snapshot_partition->size && len <= s_snapshot_partition->size - offset &&
        esp_partition_erase_range(s_snapshot_partition, offset, len) == ESP_OK;
}
static bool snapshot_write(size_t offset, const void *data, size_t len) {
    return offset <= s_snapshot_partition->size && len <= s_snapshot_partition->size - offset &&
        esp_partition_write(s_snapshot_partition, offset, data, len) == ESP_OK;
}
static void snapshot_init(void) {
    // The factory layout has an unused 64 KiB coredump data partition. This
    // profile disables coredumps and uses that space for the shutdown image.
    // Never write any application partition or disable flash write protection.
#if !CONFIG_ESP_COREDUMP_ENABLE_TO_NONE
#error "X4 power-off image storage requires coredumps disabled"
#endif
    s_snapshot_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_COREDUMP, "coredump");
    if (!s_snapshot_partition || s_snapshot_partition->address != 0xff0000 ||
        s_snapshot_partition->size != DISPLAY_SNAPSHOT_SLOT_BYTES) return;
    display_snapshot_io_t io = {snapshot_read, snapshot_erase, snapshot_write};
    s_power_image_ready = display_snapshot_open(&s_power_image, io, X4_FRAME_BYTES, 1);
    s_power_image_stored = s_power_image_ready && s_power_image.length != 0;
    ESP_LOGI(TAG, "power-off image: %s", !s_power_image_ready ? "unavailable" :
             s_power_image_stored ? "saved" : "default");
}

static bool power_pressed(void) { return gpio_get_level(PIN_POWER) == 0; }

bool xteink_x4_init(void) {
    // Never configure GPIO18/19 (native USB) or burn eFuses.
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(PIN_LATCH);
    gpio_set_level(PIN_LATCH, 1);
    if (gpio_set_direction(PIN_LATCH, GPIO_MODE_OUTPUT) != ESP_OK) return false;
    gpio_config_t power = {
        .pin_bit_mask = 1ULL << PIN_POWER, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&power) != ESP_OK) return false;
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) return false;
    adc_oneshot_chan_cfg_t channel = {
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_oneshot_config_channel(s_adc, ADC_CHANNEL_1, &channel) != ESP_OK) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return false;
    }
    button_set_power_handler(power_pressed, request_power_off);
    ESP_LOGI(TAG, "board: Xteink X4 (SSD1677); Confirm = setup, Power hold 1.5s = off");
    return true;
}

bool xteink_x4_confirm_pressed(void) {
    int raw;
    return s_adc && adc_oneshot_read(s_adc, ADC_CHANNEL_1, &raw) == ESP_OK &&
           x4_confirm_raw(raw);
}

static bool transfer(bool data, const uint8_t *bytes, size_t len) {
    gpio_set_level(PIN_DC, data);
    while (len) {
        size_t n = len > 4096 ? 4096 : len;
        spi_transaction_t t = {.length = n * 8, .tx_buffer = bytes};
        esp_err_t err = spi_device_transmit(s_spi, &t);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPI failed: %s", esp_err_to_name(err));
            return false;
        }
        bytes += n;
        len -= n;
    }
    return true;
}

static bool command(uint8_t cmd, const uint8_t *data, size_t len) {
    return transfer(false, &cmd, 1) && (!len || transfer(true, data, len));
}
static bool reg8(uint8_t cmd, uint8_t value) { return command(cmd, &value, 1); }

static bool wait_ready(void) {
    // A delayed BUSY assertion must not let us write through a refresh.
    vTaskDelay(pdMS_TO_TICKS(10));
    int64_t deadline = esp_timer_get_time() + 10000000;
    while (gpio_get_level(PIN_BUSY)) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGE(TAG, "SSD1677 BUSY timeout; stopping this update");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

static bool ram_area(void) {
    const uint8_t x[] = {0, 0, 0x1f, 3}; // 0..799
    const uint8_t y[] = {0xdf, 1, 0, 0}; // 479..0
    const uint8_t zero[] = {0, 0}, top[] = {0xdf, 1};
    return reg8(0x11, 0x01) && command(0x44, x, sizeof(x)) &&
           command(0x45, y, sizeof(y)) && command(0x4e, zero, sizeof(zero)) &&
           command(0x4f, top, sizeof(top));
}

static bool panel_reset(void) {
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    const uint8_t booster[] = {0xae, 0xc7, 0xc3, 0xc0, 0x80};
    const uint8_t gates[] = {0xdf, 1, 2};
    return command(0x12, NULL, 0) && wait_ready() && reg8(0x18, 0x80) &&
           command(0x0c, booster, sizeof(booster)) &&
           command(0x01, gates, sizeof(gates)) && reg8(0x3c, 0x80) && ram_area();
}

static bool refresh(void) {
    // Status pages are infrequent. Use the controller's full OTP waveform;
    // no custom voltages/LUTs, no shadow frame, and analog power off afterward.
    int64_t start = esp_timer_get_time();
    bool ok = panel_reset() &&
              command(0x24, s_frame, X4_FRAME_BYTES) && ram_area() &&
              command(0x26, s_frame, X4_FRAME_BYTES) &&
              reg8(0x21, 0x40) && reg8(0x3c, 0xc0) && reg8(0x22, 0xf7) &&
              command(0x20, NULL, 0) && wait_ready();
    if (ok) ok = reg8(0x10, 0x01); // Deep-sleep panel; next update resets it.
    ESP_LOGI(TAG, "refresh %s (%lld ms)", ok ? "complete" : "failed",
             (long long)((esp_timer_get_time() - start) / 1000));
    return ok;
}

static void text_at(int x, int y, const char *text, int scale) {
    int start_x = x;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '\n' || x + 6 * scale > X4_WIDTH - 24) {
            x = start_x;
            y += 10 * scale;
            if (*p == '\n') continue;
        }
        if (y + 8 * scale > X4_HEIGHT - 24) break;
        unsigned char ch = *p;
        if (ch < PIXEL_FONT_FIRST || ch > PIXEL_FONT_LAST) ch = '?';
        for (int cx = 0; cx < 5; ++cx)
            for (int cy = 0; cy < 8; ++cy)
                if (pixel_font[ch - PIXEL_FONT_FIRST][cx] & (1 << cy))
                    for (int dx = 0; dx < scale; ++dx)
                        for (int dy = 0; dy < scale; ++dy)
                            x4_pixel(s_frame, x + cx * scale + dx,
                                     y + cy * scale + dy, false);
        x += 6 * scale;
    }
}

static void show_power_off(void) {
    if (!s_ready) return;
    // Drawing/refresh is serialized with downloads. Once shutdown starts all
    // other producers stop touching the framebuffer, including in-flight images.
    xSemaphoreTake(s_frame_lock, portMAX_DELAY);
    bool custom = s_power_image_ready && display_snapshot_load(&s_power_image, s_frame);
    if (!custom) {
        memset(s_frame, 0xff, X4_FRAME_BYTES);
        text_at(28, 180, "IMPO", 10);
        text_at(28, 330, "POWER OFF", 5);
        text_at(28, 440, "Release Power.\n\nPress Power again\nto turn on.", 3);
    }
    ESP_LOGI(TAG, "shutdown page: %s", custom ? "saved image" : "default");
    refresh();
    xSemaphoreGive(s_frame_lock);
}

static esp_err_t power_off_hardware(void) {
    // FreeInk PowerManager + CrossPoint HalPowerManager: GPIO3 is active-low
    // Power; GPIO13 drops the battery latch. USB-powered units instead enter
    // deep sleep with GPIO3 as the wake source. Never erase saved credentials.
    esp_err_t err = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
        1ULL << PIN_POWER, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) return err;
    gpio_set_level(PIN_RST, 1);
    gpio_hold_en(PIN_RST); // Keep SSD1677 asleep while its rail remains powered.
    ESP_LOGI(TAG, "power off: GPIO3 wakes; battery latch off; entering deep sleep");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_LATCH, 0);
    gpio_hold_en(PIN_LATCH);
    esp_sleep_config_gpio_isolate();
    gpio_deep_sleep_hold_en();
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static void request_power_off(void) {
    const gadget_power_ops_t ops = {
        .show_off = show_power_off,
        .button_pressed = power_pressed,
        .power_off = power_off_hardware,
    };
    ESP_LOGI(TAG, "Power long press: requesting shared shutdown");
    esp_err_t err = gadget_power_off(&ops);
    ESP_LOGE(TAG, "power off refused: %s (release and try again)", esp_err_to_name(err));
    xteink_x4_show_text("Busy. Release Power,\nthen hold again to\nturn off.");
}

static const char *state_text(led_state_t state) {
    switch (state) {
    case LED_STATE_BOOT: return "Starting...";
    case LED_STATE_SETUP_IDLE:
    case LED_STATE_BLE_ADVERTISING:
    case LED_STATE_UNPAIRED: return "Ready to pair\nOpen Impo on\nyour phone.";
    case LED_STATE_BLE_CONNECTED: return "Phone connected";
    case LED_STATE_PAIRING_CONFIRM_REQUIRED: return "Press Confirm\nto allow pairing";
    case LED_STATE_WIFI_CONNECTING: return "Connecting Wi-Fi";
    case LED_STATE_WIFI_CONNECTED: return "Wi-Fi connected";
    case LED_STATE_AUTH_OK: return "Account verified";
    case LED_STATE_VM_SWITCHING: return "Changing server";
    case LED_STATE_VM_OK: return "Connecting server";
    case LED_STATE_WS_CONNECTED: return "Online";
    case LED_STATE_WS_DISCONNECTED: return "Reconnecting...";
    default: return "Connection error\nCheck Impo app";
    }
}

static void display_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500)); // Coalesce connection-state bursts.
        xSemaphoreTake(s_frame_lock, portMAX_DELAY);
        if (gadget_power_is_shutting_down()) {
            xSemaphoreGive(s_frame_lock);
            continue;
        }
        char title[sizeof(s_title)], body[sizeof(s_text)];
        bool paint = false, text_view = false, canvas = false;
        portENTER_CRITICAL(&s_meta_lock);
        if (s_canvas_pending && !s_loading && s_view == VIEW_CANVAS) {
            s_canvas_pending = false;
            s_canvas_refreshing = true;
            canvas = true;
        }
        if (s_dirty && !s_loading && (s_view == VIEW_STATUS || s_view == VIEW_TEXT)) {
            memcpy(title, s_title, sizeof(title));
            text_view = s_view == VIEW_TEXT;
            snprintf(body, sizeof(body), "%s",
                     s_view == VIEW_TEXT ? s_text : state_text(s_state));
            s_dirty = false;
            paint = true;
        }
        portEXIT_CRITICAL(&s_meta_lock);
        if (canvas) {
            refresh();
            portENTER_CRITICAL(&s_meta_lock);
            s_canvas_refreshing = false;
            portEXIT_CRITICAL(&s_meta_lock);
        }
        if (paint) {
            memset(s_frame, 0xff, X4_FRAME_BYTES);
            text_at(28, 38, "IMPO", 10);
            text_at(28, 145, "XTEINK X4", 3);
            for (int x = 28; x < X4_WIDTH - 28; ++x) x4_pixel(s_frame, x, 195, false);
            text_at(28, 230, title, 3);
            text_at(28, 310, body, text_view ? 3 : 4);
            text_at(28, 690, "Hold Power 1.5s: off\nHold Confirm 5s: reset pairing", 2);
            refresh();
        }
        xSemaphoreGive(s_frame_lock);
    }
}

static bool canvas_busy(void) {
    portENTER_CRITICAL(&s_meta_lock);
    bool busy = !s_ready || s_loading || s_canvas_pending || s_canvas_refreshing || s_power_image_busy;
    portEXIT_CRITICAL(&s_meta_lock);
    return busy || gadget_power_is_shutting_down();
}
static bool canvas_begin(void) {
    if (!s_ready || gadget_power_is_shutting_down() ||
        xSemaphoreTake(s_frame_lock, 0) != pdTRUE) return false;
    if (canvas_busy()) { xSemaphoreGive(s_frame_lock); return false; }
    portENTER_CRITICAL(&s_meta_lock);
    s_view = VIEW_CANVAS;
    s_dirty = false;
    portEXIT_CRITICAL(&s_meta_lock);
    return true;
}
static void canvas_pixel(int x, int y, bool white) { x4_pixel(s_frame, x, y, white); }
static void canvas_end(bool show) {
    portENTER_CRITICAL(&s_meta_lock);
    s_canvas_pending = show;
    portEXIT_CRITICAL(&s_meta_lock);
    xSemaphoreGive(s_frame_lock);
}
static int power_off_image_state(void) {
    portENTER_CRITICAL(&s_meta_lock);
    int state = !s_power_image_ready ? -1 : s_power_image_stored ? 1 : 0;
    portEXIT_CRITICAL(&s_meta_lock);
    return state;
}
static const display_canvas_ops_t canvas_ops = {
    .width = X4_WIDTH, .height = X4_HEIGHT, .begin = canvas_begin,
    .pixel = canvas_pixel, .end = canvas_end, .busy = canvas_busy,
    .power_off_image_state = power_off_image_state,
};

esp_err_t xteink_x4_save_power_off_image(bool reset) {
    if (!s_power_image_ready) return ESP_ERR_NOT_SUPPORTED;
    if (!s_ready || xSemaphoreTake(s_frame_lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    if (canvas_busy() || (!reset && s_view == VIEW_STATUS)) {
        xSemaphoreGive(s_frame_lock);
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_meta_lock);
    s_power_image_busy = true;
    portEXIT_CRITICAL(&s_meta_lock);
    bool ok = display_snapshot_save(&s_power_image, reset ? NULL : s_frame);
    portENTER_CRITICAL(&s_meta_lock);
    s_power_image_stored = s_power_image.length != 0;
    s_power_image_busy = false;
    portEXIT_CRITICAL(&s_meta_lock);
    xSemaphoreGive(s_frame_lock);
    ESP_LOGI(TAG, "power-off image %s: %s", reset ? "reset" : "save", ok ? "verified" : "failed");
    return ok ? ESP_OK : ESP_FAIL;
}

bool xteink_x4_begin_image(bool preserve, bool show) {
    if (!canvas_begin()) return false;
    portENTER_CRITICAL(&s_meta_lock);
    s_loading = true;
    s_image_refresh = show;
    s_image_clear = !preserve;
    s_image_changed = false;
    portEXIT_CRITICAL(&s_meta_lock);
    canvas_end(false);
    return true;
}
void xteink_x4_cancel_image(void) {
    portENTER_CRITICAL(&s_meta_lock);
    s_loading = false;
    portEXIT_CRITICAL(&s_meta_lock);
}

bool led_status_init(void) {
    gpio_hold_dis(PIN_RST);
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RST) | (1ULL << PIN_SD_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&outputs) != ESP_OK) return false;
    gpio_set_level(PIN_SD_CS, 1); // SD shares the bus; leave it deselected.
    gpio_config_t busy = {.pin_bit_mask = 1ULL << PIN_BUSY, .mode = GPIO_MODE_INPUT};
    if (gpio_config(&busy) != ESP_OK) return false;
    spi_bus_config_t bus = {.mosi_io_num = PIN_MOSI, .miso_io_num = -1,
        .sclk_io_num = PIN_SCK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 4096};
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
    spi_device_interface_config_t device = {.clock_speed_hz = 10000000, .mode = 0,
        .spics_io_num = PIN_CS, .queue_size = 1};
    if (spi_bus_add_device(SPI2_HOST, &device, &s_spi) != ESP_OK) goto fail_bus;
    s_frame = malloc(X4_FRAME_BYTES);
    s_frame_lock = xSemaphoreCreateMutex();
    if (!s_frame || !s_frame_lock) goto fail;
    memset(s_frame, 0xff, X4_FRAME_BYTES);
    snapshot_init();
    s_dirty = true;
    if (xTaskCreate(display_task, "x4_display", 4096, NULL, 1, NULL) != pdPASS) goto fail;
    s_ready = true;
    display_canvas_bind(&canvas_ops);
    ESP_LOGI(TAG, "LED status ready: Xteink X4 SSD1677, 480x800, 1 bpp");
    return true;
fail:
    free(s_frame);
    s_frame = NULL;
    if (s_frame_lock) vSemaphoreDelete(s_frame_lock);
    s_frame_lock = NULL;
    spi_bus_remove_device(s_spi);
    s_spi = NULL;
fail_bus:
    spi_bus_free(SPI2_HOST);
    return false;
}

void led_status_set_state(led_state_t state) {
    portENTER_CRITICAL(&s_meta_lock);
    if (s_state != state) { s_state = state; if (s_view == VIEW_STATUS) s_dirty = true; }
    // Pairing confirmation must remain visible even after remote content.
    if (state == LED_STATE_PAIRING_CONFIRM_REQUIRED) { s_view = VIEW_STATUS; s_dirty = true; }
    portEXIT_CRITICAL(&s_meta_lock);
}
void led_status_set_title(const char *title) {
    portENTER_CRITICAL(&s_meta_lock);
    snprintf(s_title, sizeof(s_title), "%s", title ? title : "");
    if (s_view == VIEW_STATUS) s_dirty = true;
    portEXIT_CRITICAL(&s_meta_lock);
}
bool led_status_display_info(int *width, int *height) {
    if (width) *width = X4_WIDTH;
    if (height) *height = X4_HEIGHT;
    return s_ready;
}
int led_status_display_bits(void) { return 1; }
bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels) {
    if (!s_ready || gadget_power_is_shutting_down() || !pixels || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x >= X4_WIDTH || y >= X4_HEIGHT || w > X4_WIDTH - x ||
        h > X4_HEIGHT - y || (size_t)w * h > X4_WIDTH * 23) return false;
    xSemaphoreTake(s_frame_lock, portMAX_DELAY);
    if (gadget_power_is_shutting_down()) {
        xSemaphoreGive(s_frame_lock);
        return false;
    }
    portENTER_CRITICAL(&s_meta_lock);
    bool first = !s_loading;
    s_loading = true;
    if (first) s_view = VIEW_IMAGE;
    bool clear = first || s_image_clear;
    s_image_clear = false;
    s_image_changed = true;
    portEXIT_CRITICAL(&s_meta_lock);
    if (clear) memset(s_frame, 0xff, X4_FRAME_BYTES);
    bool ok = x4_draw_rgb565(s_frame, x, y, w, h, (const uint8_t *)pixels);
    xSemaphoreGive(s_frame_lock);
    return ok;
}
void led_status_draw_done(void) {
    if (!s_ready) return;
    xSemaphoreTake(s_frame_lock, portMAX_DELAY);
    portENTER_CRITICAL(&s_meta_lock);
    bool paint = s_loading && s_image_changed &&
        (s_view == VIEW_IMAGE || s_view == VIEW_CANVAS) && s_image_refresh;
    portEXIT_CRITICAL(&s_meta_lock);
    if (paint && !gadget_power_is_shutting_down()) refresh();
    portENTER_CRITICAL(&s_meta_lock);
    s_loading = false;
    portEXIT_CRITICAL(&s_meta_lock);
    xSemaphoreGive(s_frame_lock);
}
void led_status_show_animation(void) {
    portENTER_CRITICAL(&s_meta_lock);
    if (s_loading) { portEXIT_CRITICAL(&s_meta_lock); return; }
    s_view = VIEW_STATUS;
    s_dirty = true;
    s_canvas_pending = false;
    portEXIT_CRITICAL(&s_meta_lock);
}
bool xteink_x4_show_text(const char *text) {
    if (!s_ready || gadget_power_is_shutting_down() || !text) return false;
    portENTER_CRITICAL(&s_meta_lock);
    bool ok = !s_loading && !s_canvas_pending && !s_canvas_refreshing && !s_power_image_busy;
    if (ok) {
        snprintf(s_text, sizeof(s_text), "%s", text);
        s_view = VIEW_TEXT;
        s_dirty = true;
    }
    portEXIT_CRITICAL(&s_meta_lock);
    return ok;
}
void led_status_set_voice(led_voice_t voice) { (void)voice; }
void led_status_set_level(float level) { (void)level; }
void led_status_show_volume(int percent) { (void)percent; }
