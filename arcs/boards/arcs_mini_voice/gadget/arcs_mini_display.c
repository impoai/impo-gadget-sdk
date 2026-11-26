/* SPDX-License-Identifier: MIT
 * Status rendering is off the input/link/audio tasks. The framebuffer lives in PSRAM.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "lisa_display.h"
#include "lisa_gpio.h"
#include "shell.h"
#include "platform_arcs.h"
#include "pinmux.h"
#include "arcs_mini_display.h"
#include "arcs_mini_screen_ui.h"
#include "gadget_voice.h"
#define TAG "arcs_mini_display"
#include "lisa_log.h"

static volatile gadget_status_t s_requested = GADGET_STATUS_BOOTING;
static volatile bool s_ready, s_color_test;
static volatile uint32_t s_test_at, s_frames;
static volatile int s_error;
void arcs_mini_display_show(gadget_status_t status) { s_requested = status; }
void arcs_mini_display_restore(void) { s_color_test = false; }

static void display_task(void *arg)
{
    (void)arg;
    lisa_device_t *dev = lisa_device_get("display");
    lisa_device_t *a = lisa_device_get("gpioa"), *b = lisa_device_get("gpiob");
    lisa_device_t *spi = lisa_device_get("spi0"), *pwm = lisa_device_get("pwm0");
    if (!lisa_device_ready(dev) || !lisa_device_ready(a) || !lisa_device_ready(b) ||
        !lisa_device_ready(spi) || !lisa_device_ready(pwm)) {
        s_error = -1; LISA_LOGE(TAG, "display dependency not ready"); vTaskDelete(NULL); return;
    }
    lisa_display_config_t cfg = {
        .panel_name = "st7789p3", .bus_type = LISA_DISPLAY_BUS_SPI_4WIRE,
        .bus_config.spi_4wire = { .spi_dev = spi, .cs_gpio = a, .cs_pin = LCD_CS_PIN,
                                 .dc_gpio = a, .dc_pin = LCD_CD_PIN, .spi_freq = 20000000 },
        .rst_gpio = b, .rst_pin = LCD_RST_PIN,
        .backlight = { .type = LISA_DISPLAY_BACKLIGHT_TYPE_PWM,
            .blacklight_polarity = LISA_DISPLAY_BLACKLIGHT_POLARITY_HIGH,
            .config.pwm = { .dev = pwm, .channel = 1, .freq = 2000 } }
    };
    int r = lisa_display_attach_bus(dev, &cfg);
    lisa_display_capabilities_t caps = {0};
    if (!r) r = lisa_display_get_capabilities(dev, &caps);
    if (!r && (caps.width != ARCS_MINI_SCREEN_WIDTH || caps.height != ARCS_MINI_SCREEN_HEIGHT)) r = -2;
    uint16_t *pixels = r ? NULL : malloc(ARCS_MINI_SCREEN_WIDTH*ARCS_MINI_SCREEN_HEIGHT*sizeof(uint16_t));
    if (!r && !pixels) r = -3;
    if (!r) r = lisa_display_blanking_off(dev);
    if (!r) r = lisa_display_set_brightness(dev, 70);
    if (r) {
        s_error = r; free(pixels); LISA_LOGE(TAG, "display start failed: %d", r); vTaskDelete(NULL); return;
    }
    s_ready = true;
    LISA_LOGI(TAG, "ST7789P3 ready: %ux%u, SPI 20 MHz, brightness 70%%", caps.width, caps.height);
    lisa_display_buffer_desc_t desc = { .width = ARCS_MINI_SCREEN_WIDTH, .height = ARCS_MINI_SCREEN_HEIGHT,
        .pitch = ARCS_MINI_SCREEN_WIDTH, .buf_size = ARCS_MINI_SCREEN_WIDTH*ARCS_MINI_SCREEN_HEIGHT*2 };
    int previous = -1;
    uint32_t previous_phase = UINT32_MAX;
    bool previous_test = false, previous_wake = false, previous_dialogue = false;
    for (;;) {
        uint32_t now = (uint32_t)plat_uptime_ms();
        if (s_color_test && now-s_test_at >= 15000) s_color_test = false;
        gadget_status_t state = s_requested;
        bool animated = state == GADGET_STATUS_LISTENING || state == GADGET_STATUS_THINKING ||
                        state == GADGET_STATUS_SPEAKING || state == GADGET_STATUS_CONNECTING;
        uint32_t phase = animated ? now/400 : now%5000 >= 4800;
        bool test = s_color_test, wake = gadget_voice_wakeup_available(), dialogue = gadget_voice_continuous_active();
        if (previous != (int)state || previous_phase != phase || previous_test != test || previous_wake != wake || previous_dialogue != dialogue) {
            arcs_mini_screen_render(pixels, caps.width, caps.height, state, now, test, wake, dialogue);
            r = lisa_display_write(dev, 0, 0, &desc, pixels);
            if (r) { s_error = r; LISA_LOGE(TAG, "frame write failed: %d", r); }
            else { s_error = 0; s_frames++; }
            previous = state; previous_phase = phase; previous_test = test; previous_wake = wake; previous_dialogue = dialogue;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
int arcs_mini_display_start(void)
{
    if (xTaskCreate(display_task, "arcs_mini_screen", 4096/sizeof(StackType_t), NULL, 1, NULL) == pdPASS) return 0;
    s_error = -4;
    LISA_LOGE(TAG, "display task allocation failed");
    return -1;
}
static int screen_cmd(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "test") == 0) {
        s_test_at = (uint32_t)plat_uptime_ms(); s_color_test = true;
    }
    shellPrint(shellGetCurrent(), "screen ready=%u frames=%u status=%u error=%d size=240x240\n",
        (unsigned)s_ready, (unsigned)s_frames, (unsigned)s_requested, s_error);
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 screen, screen_cmd, screen status or 15-second color test);
