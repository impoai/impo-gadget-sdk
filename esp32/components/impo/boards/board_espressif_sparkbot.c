/*
 * Copyright (c) Impo contributors.
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

/*
 * Espressif ESP-SparkBot: ESP32-S3 with 8 MB PSRAM and 16 MB flash, a 240 px
 * ST7789 LCD (no touch screen), an ES8311 codec with one mic, an OV2640
 * camera, a BMI270 motion sensor, three capacitive pads and the BOOT button.
 * Pins follow xiaozhi-esp32's board, main/boards/espressif/esp-sparkbot
 * (config.h and esp_sparkbot_board.cc), and the vendor's factory demo for
 * the rest. GPIO46 is both the backlight and the amplifier enable, so it is
 * only ever on or off, never dimmed, and the codec is not given the pin.
 *
 * The top pad is the talk button, the side pads the aux button; BOOT, hidden
 * under the shell, is the talk button too. The optional tracked base listens
 * on UART1 for the drive, dance and light commands its own firmware defines,
 * and stops itself half a second after the last one, so a drive is kept up
 * by a task here that repeats it until its time is over.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "camera.h"
#include "camera_dvp.h"
#include "impo_audio.h"
#include "impo_board.h"
#include "impo_camera.h"
#include "impo_mem.h"
#include "sparkbot_console.h"
#include "sparkbot_imu.h"
#include "sparkbot_touch.h"

static const char *TAG = "board";

#define LCD_RES 240
#define LCD_HOST SPI3_HOST
#define LCD_SCLK GPIO_NUM_21
#define LCD_MOSI GPIO_NUM_47
#define LCD_CS GPIO_NUM_44
#define LCD_DC GPIO_NUM_43
#define LCD_BL GPIO_NUM_46          /* also the amplifier enable */
#define DRAW_BUF_LINES 40

#define I2C_SDA GPIO_NUM_4
#define I2C_SCL GPIO_NUM_5
#define I2S_MCLK GPIO_NUM_45
#define I2S_WS GPIO_NUM_41
#define I2S_BCLK GPIO_NUM_39
#define I2S_DIN GPIO_NUM_40
#define I2S_DOUT GPIO_NUM_42

#define TALK_GPIO GPIO_NUM_0        /* BOOT */
#define TOUCH_WAIT_MS 150           /* asleep, how often the pads are looked at */

#define CAM_XCLK GPIO_NUM_15
#define CAM_PCLK GPIO_NUM_13
#define CAM_VSYNC GPIO_NUM_6
#define CAM_HREF GPIO_NUM_7
#define CAM_DATA { GPIO_NUM_11, GPIO_NUM_9, GPIO_NUM_8, GPIO_NUM_10, GPIO_NUM_12, GPIO_NUM_18, GPIO_NUM_17, GPIO_NUM_16 }

/* Battery through a 100k/100k divider on GPIO14; the factory demo's meter. */
#define BATT_ADC_UNIT ADC_UNIT_2
#define BATT_ADC ADC_CHANNEL_3
#define BATT_EMPTY_MV 3100
#define BATT_FULL_MV 4200
#define BATT_ABSENT_MV 2500         /* below this nothing is connected */

#define BASE_UART UART_NUM_1         /* the tracked base, when one is attached */
#define BASE_TX GPIO_NUM_38
#define BASE_RX GPIO_NUM_48
#define BASE_TICK_MS 200             /* repeat a drive this often; the base stops after 500 */
#define BASE_STEP_MS 500             /* one "step" */
#define BASE_MAX_MS 10000

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_handle_t s_panel;
static impo_gpio_button_t s_talk;
static unsigned s_touch;             /* pads down at the last poll */
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static bool s_imu_ok;

/* The base: what it was last told, how long to keep saying it, and whether
 * it has echoed anything back lately. All under s_base_lock. */
static SemaphoreHandle_t s_base_lock;
static struct {
    float x, y;
    TickType_t until;
    bool moving;            /* a non-zero drive was sent and no stop since */
    bool ever_sent;
    TickType_t asked;       /* the first send since the base last echoed */
    bool answered;          /* an echo has come since `asked` */
} s_base;

static void base_task(void *arg);

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    const uart_config_t base_cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(BASE_UART, 2048, 0, 0, NULL, 0), TAG, "base uart");
    ESP_RETURN_ON_ERROR(uart_param_config(BASE_UART, &base_cfg), TAG, "base uart config");
    ESP_RETURN_ON_ERROR(uart_set_pin(BASE_UART, BASE_TX, BASE_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "base uart pins");
    s_base_lock = xSemaphoreCreateMutex();
    if (!s_base_lock) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(impo_gpio_button_init(&s_talk, TALK_GPIO), TAG, "boot button");
    /* Without the pads BOOT still talks; without the IMU its command says so. */
    esp_err_t err = sparkbot_touch_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch pads: %s", esp_err_to_name(err));
    }
    s_imu_ok = sparkbot_imu_init(s_i2c) == ESP_OK;
    if (s_imu_ok) {
        sparkbot_imu_watch_start();   /* shaken, picked up, put down: link.event */
    }

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = BATT_ADC_UNIT };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATT_ADC, &ch_cfg), TAG, "adc channel");
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BATT_ADC_UNIT, .chan = BATT_ADC, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) != ESP_OK) {
        s_adc_cali = NULL;   /* raw counts then, see read_power */
    }

    const camera_dvp_config_t cam = {
        .name = "ESP-SparkBot camera",
        .xclk = CAM_XCLK, .pclk = CAM_PCLK, .vsync = CAM_VSYNC, .href = CAM_HREF,
        .d = CAM_DATA,
        .i2c_port = I2C_NUM_0,
        .vflip = true,
    };
    camera_register(camera_dvp(&cam));

    BaseType_t ok = xTaskCreatePinnedToCore(base_task, "base", 3072, NULL, 5, NULL, tskNO_AFFINITY);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    (void)touch;
    const gpio_config_t bl = {
        .pin_bit_mask = 1ULL << LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&bl) != ESP_OK) {
        return NULL;
    }
    gpio_set_level(LCD_BL, 0);

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_RES * DRAW_BUF_LINES * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = IMPO_UI_CORE;
    adapter_cfg.task_priority = IMPO_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_RES,
            .ver_res = LCD_RES,
            .buffer_height = DRAW_BUF_LINES,
            .use_psram = true,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    /* On or off only: the pin also enables the amplifier. */
    gpio_set_level(LCD_BL, pct > 0);
}

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep);   /* SLPIN/SLPOUT; GRAM is kept */
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

/* One ES8311 does both directions over a duplex I2S bus, clocked from MCLK as xiaozhi does. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(IMPO_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = GPIO_NUM_NC,     /* shared with the backlight, see set_brightness() */
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

/* BOOT and the top pad both talk; the side pads are the aux button. */
static unsigned poll_buttons(void)
{
    unsigned ev = impo_gpio_button_poll(&s_talk);
    unsigned now = sparkbot_touch_state();
    unsigned was = s_touch;
    s_touch = now;
    bool top_now = now & SPARKBOT_TOUCH_TOP, top_was = was & SPARKBOT_TOUCH_TOP;
    bool side_now = now & (SPARKBOT_TOUCH_LEFT | SPARKBOT_TOUCH_RIGHT);
    bool side_was = was & (SPARKBOT_TOUCH_LEFT | SPARKBOT_TOUCH_RIGHT);
    if (top_now != top_was) {
        ev |= top_now ? IMPO_BTN_TALK_PRESS : IMPO_BTN_TALK_RELEASE;
    }
    if (side_now != side_was) {
        ev |= side_now ? IMPO_BTN_AUX_PRESS : IMPO_BTN_AUX_RELEASE;
    }
    return ev;
}

/* BOOT interrupts; the pads are looked at between short sleeps. */
static void wait_buttons(int timeout_ms)
{
    if (sparkbot_touch_state() != s_touch) {
        return;
    }
    impo_gpio_buttons_wait((impo_gpio_button_t *const[]){ &s_talk }, 1,
                           timeout_ms < TOUCH_WAIT_MS ? timeout_ms : TOUCH_WAIT_MS);
}

static esp_err_t read_power(impo_power_t *out)
{
    int sum = 0;
    for (int i = 0; i < 4; i++) {
        int v;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATT_ADC, &v), TAG, "adc read");
        sum += v;
    }
    int raw = sum / 4, mv = 0;
    if (s_adc_cali) {
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_adc_cali, raw, &mv), TAG, "adc cali");
    } else {
        mv = raw * 3100 / 4095;   /* 12 dB attenuation, roughly */
    }
    mv *= 2;   /* the divider */
    out->usb = usb_serial_jtag_is_connected();
    out->charging = false;   /* the charger's status line isn't on a pin */
    out->battery_mv = mv;
    if (mv < BATT_ABSENT_MV) {
        out->battery_pct = -1;
        return ESP_OK;
    }
    int pct = (mv - BATT_EMPTY_MV) * 100 / (BATT_FULL_MV - BATT_EMPTY_MV);
    out->battery_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    return ESP_OK;
}

/* There is no power latch to open: sleep until BOOT is pressed. */
static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    while (gpio_get_level(TALK_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static void snap_task(void *arg)
{
    char *jpeg = NULL;
    const char *error = NULL;
    int64_t started = esp_timer_get_time();
    if (impo_camera_capture(&jpeg, &error)) {
        int64_t captured = esp_timer_get_time();
        printf("@snap {\"jpeg_base64\":\"%s\"}\n", jpeg);
        fflush(stdout);
        ESP_LOGI(TAG, "snap: capture %lld ms, output %lld ms", (long long)((captured - started) / 1000),
                 (long long)((esp_timer_get_time() - captured) / 1000));
    } else {
        printf("@snap {\"error\":\"%s\"}\n", error);
    }
    free(jpeg);
    fflush(stdout);
    xTaskNotifyGive((TaskHandle_t)arg);
    vTaskDeleteWithCaps(NULL);
}

/* ">snap": one photo as base64, for tools/impo/photo.py, on a task with room
 * for the sensor driver and the JPEG encoder, which the serial task hasn't. */
static void console_snap(void)
{
    TaskHandle_t waiter = xTaskGetCurrentTaskHandle();
    if (xTaskCreateWithCaps(snap_task, "snap", 8192, waiter, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
    } else {
        printf("@snap {\"error\":\"out of memory\"}\n");
        fflush(stdout);
    }
}

/* With s_base_lock held: notes any echo the base sent since the last look.
 * The base's firmware echoes every byte. */
static void base_listen(void)
{
    size_t waiting = 0;
    if (uart_get_buffered_data_len(BASE_UART, &waiting) == ESP_OK && waiting > 0) {
        uart_flush_input(BASE_UART);
        s_base.answered = true;
    }
}

/* With s_base_lock held: one line to the base. */
static bool base_write(const char *text)
{
    base_listen();
    if (!s_base.ever_sent || s_base.answered) {
        s_base.asked = xTaskGetTickCount();
    }
    s_base.ever_sent = true;
    s_base.answered = false;
    return uart_write_bytes(BASE_UART, text, strlen(text)) == (int)strlen(text);
}

static bool base_write_drive(float x, float y)
{
    char text[32];
    snprintf(text, sizeof(text), "x%.2f y%.2f", x, y);
    return base_write(text);
}

/* Keeps a drive going until its time is up, then stops the base once. */
static void base_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BASE_TICK_MS));
        if (xSemaphoreTake(s_base_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(s_base.until - now) > 0) {
            base_write_drive(s_base.x, s_base.y);
        } else if (s_base.moving) {
            base_write_drive(0, 0);
            s_base.moving = false;
        }
        xSemaphoreGive(s_base_lock);
    }
}

/* With the lock held: "responding" (what was last sent was echoed), "silent"
 * (sent to, nothing back) or "unknown" (nothing sent yet, or the echo isn't
 * due). */
static const char *base_presence(void)
{
    base_listen();
    if (!s_base.ever_sent) {
        return "unknown";
    }
    if (s_base.answered) {
        return "responding";
    }
    if (xTaskGetTickCount() - s_base.asked < pdMS_TO_TICKS(BASE_TICK_MS)) {
        return "unknown";
    }
    return "silent";
}

/* The result of a command that spoke to the base. */
static cJSON *base_result(bool sent)
{
    if (!sent) {
        return impo_command_error("uart_error", "the base command could not be transmitted");
    }
    cJSON *result = impo_command_ok();
    cJSON_AddStringToObject(result, "base", base_presence());
    cJSON_AddBoolToObject(result, "moving", s_base.moving);
    return result;
}

static cJSON *base_busy(void)
{
    return impo_command_error("busy", "a local base diagnostic is running");
}

/* A drive for `ms`: the task repeats it and stops the base afterwards. */
static cJSON *base_drive(float x, float y, int ms)
{
    if (xSemaphoreTake(s_base_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return base_busy();
    }
    bool stop = x == 0 && y == 0;
    s_base.x = x;
    s_base.y = y;
    s_base.until = stop ? 0 : xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    bool sent = base_write_drive(x, y);
    s_base.moving = !stop;
    cJSON *result = base_result(sent);
    xSemaphoreGive(s_base_lock);
    return result;
}

static cJSON *base_say(const char *text)
{
    if (xSemaphoreTake(s_base_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return base_busy();
    }
    cJSON *result = base_result(base_write(text));
    xSemaphoreGive(s_base_lock);
    return result;
}

static int duration_param(const cJSON *params, int fallback)
{
    const cJSON *ms = cJSON_GetObjectItem(params, "duration_ms");
    if (!cJSON_IsNumber(ms)) {
        return fallback;
    }
    return ms->valueint < 0 ? -1 : ms->valueint > BASE_MAX_MS ? BASE_MAX_MS : ms->valueint;
}

static bool axis_param(const cJSON *params, const char *name, float *out)
{
    const cJSON *v = cJSON_GetObjectItem(params, name);
    if (!v) {
        *out = 0;
        return true;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble < -1 || v->valuedouble > 1) {
        return false;
    }
    *out = (float)v->valuedouble;
    return true;
}

static cJSON *chassis_drive(const cJSON *params)
{
    float x, y;
    if (!axis_param(params, "turn", &x) || !axis_param(params, "forward", &y)) {
        return impo_command_error("invalid_param", "forward and turn must be -1 to 1");
    }
    int ms = duration_param(params, BASE_STEP_MS);
    if (ms < 0) {
        return impo_command_error("invalid_param", "duration_ms must be 0 to 10000");
    }
    return base_drive(x, y, ms);
}

static cJSON *chassis_move(const cJSON *params)
{
    static const struct {
        const char *direction;
        float x, y;
    } moves[] = { { "forward", 0, 1 }, { "back", 0, -1 }, { "left", -1, 0 }, { "right", 1, 0 }, { "stop", 0, 0 } };
    const char *direction = cJSON_GetStringValue(cJSON_GetObjectItem(params, "direction"));
    int ms = duration_param(params, BASE_STEP_MS);
    if (ms < 0) {
        return impo_command_error("invalid_param", "duration_ms must be 0 to 10000");
    }
    for (size_t i = 0; direction && i < sizeof(moves) / sizeof(moves[0]); i++) {
        if (!strcmp(direction, moves[i].direction)) {
            return base_drive(moves[i].x, moves[i].y, ms);
        }
    }
    return impo_command_error("invalid_param", "direction must be forward, back, left, right or stop");
}

static cJSON *chassis_stop(const cJSON *params)
{
    (void)params;
    return base_drive(0, 0, 0);
}

static cJSON *chassis_dance(const cJSON *params)
{
    (void)params;
    return base_say("d1");
}

/* The base's light_mode_t, by name; its first two are its own charging states. */
static cJSON *chassis_light(const cJSON *params)
{
    static const struct {
        const char *effect;
        int mode;
    } effects[] = { { "on", 2 }, { "blink", 3 }, { "breathe_slow", 4 }, { "breathe_fast", 5 },
                    { "flowing", 6 }, { "show", 7 }, { "off", 8 } };
    const char *effect = cJSON_GetStringValue(cJSON_GetObjectItem(params, "effect"));
    for (size_t i = 0; effect && i < sizeof(effects) / sizeof(effects[0]); i++) {
        if (!strcmp(effect, effects[i].effect)) {
            char text[8];
            snprintf(text, sizeof(text), "w%d", effects[i].mode);
            return base_say(text);
        }
    }
    return impo_command_error("invalid_param", "effect must be on, blink, breathe_slow, breathe_fast, flowing, show or off");
}

static cJSON *chassis_status(const cJSON *params)
{
    (void)params;
    if (xSemaphoreTake(s_base_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return base_busy();
    }
    cJSON *result = impo_command_ok();
    cJSON_AddStringToObject(result, "base", base_presence());
    cJSON_AddBoolToObject(result, "moving", s_base.moving);
    xSemaphoreGive(s_base_lock);
    return result;
}

static cJSON *imu_read(const cJSON *params)
{
    (void)params;
    sparkbot_imu_reading_t r;
    if (!s_imu_ok) {
        return impo_command_error("unavailable", "the motion sensor did not start");
    }
    esp_err_t err = sparkbot_imu_read(&r);
    if (err != ESP_OK) {
        return impo_command_error("sensor_error", esp_err_to_name(err));
    }
    cJSON *result = impo_command_ok();
    cJSON_AddStringToObject(result, "orientation", r.orientation);
    cJSON_AddBoolToObject(result, "moving", r.moving);
    cJSON *accel = cJSON_AddObjectToObject(result, "accel_g");
    cJSON_AddNumberToObject(accel, "x", round(r.accel[0] * 100.0) / 100.0);
    cJSON_AddNumberToObject(accel, "y", round(r.accel[1] * 100.0) / 100.0);
    cJSON_AddNumberToObject(accel, "z", round(r.accel[2] * 100.0) / 100.0);
    cJSON *gyro = cJSON_AddObjectToObject(result, "gyro_dps");
    cJSON_AddNumberToObject(gyro, "x", round(r.gyro[0]));
    cJSON_AddNumberToObject(gyro, "y", round(r.gyro[1]));
    cJSON_AddNumberToObject(gyro, "z", round(r.gyro[2]));
    return result;
}

/* USB diagnostics: ">chassis=forward" and the like, ">imu", ">touch" and ">snap". */
bool impo_sparkbot_console(const char *line, bool whole)
{
    if (whole && !strcmp(line, "imu")) {
        sparkbot_imu_reading_t r;
        esp_err_t err = s_imu_ok ? sparkbot_imu_read(&r) : ESP_ERR_NOT_FOUND;
        if (err != ESP_OK) {
            printf("@imu {\"error\":\"%s\"}\n", esp_err_to_name(err));
        } else {
            printf("@imu {\"orientation\":\"%s\",\"moving\":%s,\"accel_g\":[%.2f,%.2f,%.2f],"
                   "\"gyro_dps\":[%.0f,%.0f,%.0f]}\n", r.orientation, r.moving ? "true" : "false",
                   r.accel[0], r.accel[1], r.accel[2], r.gyro[0], r.gyro[1], r.gyro[2]);
        }
        fflush(stdout);
        return true;
    }
    if (whole && !strcmp(line, "snap")) {
        console_snap();
        return true;
    }
    if (whole && !strcmp(line, "touch")) {
        printf("@touch {\"pads\":%u}\n", sparkbot_touch_state());
        fflush(stdout);
        return true;
    }
    if (strncmp(line, "chassis=", 8)) {
        return false;
    }
    const char *direction = line + 8;
    if (whole && !strncmp(direction, "light=", 6)) {
        /* The base's light effect, by the remote command's name. */
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "effect", direction + 6);
        cJSON *result = chassis_light(params);
        cJSON_Delete(params);
        char *text = cJSON_PrintUnformatted(result);
        printf("@chassis %s\n", text ? text : "{}");
        free(text);
        cJSON_Delete(result);
        fflush(stdout);
        return true;
    }
    static const struct {
        const char *direction;
        float x, y;
    } moves[] = { { "forward", 0, 1 }, { "back", 0, -1 }, { "left", -1, 0 }, { "right", 1, 0 },
                  { "stop", 0, 0 }, { "probe", 0, 0 } };
    const float *move = NULL;
    int ms = 300;   /* "forward:1000" holds it longer, up to two seconds */
    const char *colon = strchr(direction, ':');
    size_t name_len = colon ? (size_t)(colon - direction) : strlen(direction);
    if (colon) {
        ms = atoi(colon + 1);
        ms = ms < 0 ? 0 : ms > 2000 ? 2000 : ms;
    }
    for (size_t i = 0; whole && i < sizeof(moves) / sizeof(moves[0]); ++i) {
        if (strlen(moves[i].direction) == name_len && !strncmp(direction, moves[i].direction, name_len)) {
            move = &moves[i].x;
        }
    }
    if (!move) {
        printf("@chassis {\"error\":\"use chassis=probe|stop|forward|back|left|right[:ms]|light=<effect>\"}\n");
        fflush(stdout);
        return true;
    }
    /* A pulse, held by the task like a remote drive, then a look at the echo. */
    cJSON *result = base_drive(move[0], move[1], ms);
    bool sent = cJSON_IsTrue(cJSON_GetObjectItem(result, "ok"));
    cJSON_Delete(result);
    vTaskDelay(pdMS_TO_TICKS(ms + 100));
    if (xSemaphoreTake(s_base_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        printf("@chassis {\"direction\":\"%.*s\",\"pulse_ms\":%d,\"sent\":%s,\"base\":\"%s\"}\n",
               (int)name_len, direction, ms, sent ? "true" : "false", base_presence());
        xSemaphoreGive(s_base_lock);
    } else {
        printf("@chassis {\"error\":\"busy\"}\n");
    }
    fflush(stdout);
    return true;
}

static const impo_command_t s_commands[] = {
    { "chassis.drive",
      "Drive the SparkBot's tracked base, if it is sitting on one, for a while: forward speed and "
      "turn, each -1 to 1 (turn with forward 0 spins in place). It stops by itself when the time "
      "is up. The result says whether the base answered.",
      NULL,
      "{\"forward\":{\"type\":\"number\",\"description\":\"-1 (full speed back) to 1 (full speed forward); 0 if left out.\"},"
      "\"turn\":{\"type\":\"number\",\"description\":\"-1 (left) to 1 (right); 0 if left out.\"},"
      "\"duration_ms\":{\"type\":\"integer\",\"description\":\"How long, up to 10000; 500 if left out.\"}}",
      chassis_drive },
    { "chassis.move",
      "Move the SparkBot's tracked base, if it is sitting on one, in one direction for a while "
      "(half a second if no duration), or stop it.",
      "{\"direction\":{\"type\":\"string\",\"description\":\"forward, back, left, right or stop.\"}}",
      "{\"duration_ms\":{\"type\":\"integer\",\"description\":\"How long, up to 10000; 500 if left out.\"}}",
      chassis_move },
    { "chassis.stop", "Stop the SparkBot's tracked base at once.", NULL, NULL, chassis_stop },
    { "chassis.dance", "Make the SparkBot's tracked base do its own little dance, a few seconds long.",
      NULL, NULL, chassis_dance },
    { "chassis.set_light",
      "Choose the effect on the tracked base's RGB lights.",
      "{\"effect\":{\"type\":\"string\",\"description\":\"on, blink, breathe_slow, breathe_fast, flowing, show or off.\"}}",
      NULL, chassis_light },
    { "chassis.status",
      "Whether a tracked base is answering the SparkBot and whether it is moving.",
      NULL, NULL, chassis_status },
    { "imu.read",
      "Read the SparkBot's motion sensor: which way up it is (upright, upside_down, face_up, "
      "face_down, on_left_side, on_right_side or tilted), whether it is being moved, and the raw "
      "acceleration in g and rotation in degrees per second.",
      NULL, NULL, imu_read },
};

/* What the agent is told about this device (CAPABILITIES.md, "How a gadget
 * says what it has"). The person's own instructions come first. */
static const impo_event_t s_events[] = {
    { "picked_up", "Someone lifted the robot off the desk, most likely to talk to it or take it along." },
    { "put_down", "The robot was set back down on a flat surface." },
    { "shaken", "The robot was shaken; strength is gentle or hard. A hard shake means someone wants its attention now." },
};

static const char s_instructions[] =
    "This gadget is a small desk robot with a face, a camera and a speaker, on a "
    "tracked base. When it is picked up, say exactly, in Chinese: 你拿我干嘛？ and "
    "nothing else. When it is put down, say one short friendly sentence. When it "
    "is shaken, tell the person how many unread emails they have and the subjects "
    "of the newest two if their mail is connected; otherwise say what you'd need. "
    "Everything you say through it is spoken aloud: keep to three sentences, no "
    "markdown, lists or links.";

static const impo_board_t s_board = {
    .name = "ESP-SparkBot",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = false,
    .touch = false,
    .diagonal_in = 1.54f,
    .talk_button = "top",
    .aux_button = "side",
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,              /* one mic, on the left slot */
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
    .commands = s_commands,
    .command_count = sizeof(s_commands) / sizeof(s_commands[0]),
    .features = "motion_sensor,touch_pads,locomotion,lights",
    .events = s_events,
    .event_count = sizeof(s_events) / sizeof(s_events[0]),
    .instructions = s_instructions,
};

/* Home Link's app_main starts Impo with this board (main/main.c). */
const impo_board_t *impo_board_get(void)
{
    return &s_board;
}
