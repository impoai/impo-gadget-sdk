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
#include "camera_dvp.h"

#include <stdlib.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "camera_dvp";

/* VGA JPEG, 30-60 KB: enough to see what's in front of the gadget, small
 * enough to go up the Link as base64. */
#define FRAME_SIZE FRAMESIZE_VGA
#define JPEG_QUALITY 12             /* the sensor's scale, 0 best */
#define JPEG_QUALITY_ENCODE 80      /* esp_new_jpeg's scale, 100 best */
/* The sensor's exposure settles over its first frames after a cold start:
 * about half a second's worth, more in a dark room. */
#define WARMUP_FRAMES 10
#define WARMUP_DELAY_MS 60

static camera_dvp_config_t s_cfg;
/* Whether the sensor makes JPEG itself (an OV2640) or hands over RGB565 to be
 * encoded here (a GC2145); found out at the first capture. */
static enum { JPEG_UNKNOWN, JPEG_NATIVE, JPEG_ENCODED } s_jpeg;

static esp_err_t sensor_start(bool native_jpeg)
{
    camera_config_t cfg = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = s_cfg.xclk,
        .pin_sccb_sda = -1,
        .pin_sccb_scl = -1,
        .pin_d7 = s_cfg.d[7], .pin_d6 = s_cfg.d[6], .pin_d5 = s_cfg.d[5], .pin_d4 = s_cfg.d[4],
        .pin_d3 = s_cfg.d[3], .pin_d2 = s_cfg.d[2], .pin_d1 = s_cfg.d[1], .pin_d0 = s_cfg.d[0],
        .pin_vsync = s_cfg.vsync,
        .pin_href = s_cfg.href,
        .pin_pclk = s_cfg.pclk,
        .xclk_freq_hz = 16 * 1000 * 1000,   /* EDMA into PSRAM, as the vendor's demo clocks it */
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = native_jpeg ? PIXFORMAT_JPEG : PIXFORMAT_RGB565,
        .frame_size = FRAME_SIZE,
        .jpeg_quality = JPEG_QUALITY,
        .fb_count = 2,                      /* DMA always has somewhere to write */
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
        .sccb_i2c_port = s_cfg.i2c_port,
    };
    ESP_RETURN_ON_ERROR(esp_camera_init(&cfg), TAG, "init");
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, s_cfg.vflip);
        s->set_hmirror(s, s_cfg.hmirror);
    }
    return ESP_OK;
}

typedef struct {
    uint8_t *jpeg;      /* a copy of the sensor's JPEG, or one encoded here */
} frame_priv_t;

/* Pixels copied 16-byte aligned, as the encoder wants them, in PSRAM. */
static uint8_t *copy_of(const uint8_t *data, size_t len)
{
    uint8_t *copy = jpeg_calloc_align(len, 16);
    if (copy) {
        memcpy(copy, data, len);
    }
    return copy;
}

/*
 * RGB565 pixels (the sensor's byte order, high byte first) to a JPEG with
 * esp_new_jpeg, Espressif's encoder built on the S3's SIMD instructions: a
 * VGA frame in a quarter of a second. `out` is the caller's to free.
 */
static esp_err_t encode(const uint8_t *aligned, size_t len, int width, int height, uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = width;
    cfg.height = height;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality = JPEG_QUALITY_ENCODE;
    size_t cap = (size_t)width * height / 2;   /* ample at this quality */
    uint8_t *jpeg = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg) {
        return ESP_ERR_NO_MEM;
    }
    jpeg_enc_handle_t enc = NULL;
    jpeg_error_t err = jpeg_enc_open(&cfg, &enc);
    int produced = 0;
    if (err == JPEG_ERR_OK) {
        err = jpeg_enc_process(enc, aligned, len, jpeg, cap, &produced);
        jpeg_enc_close(enc);
    }
    if (err != JPEG_ERR_OK || produced <= 0) {
        free(jpeg);
        ESP_LOGW(TAG, "encode: %d", (int)err);
        return err == JPEG_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    *out = jpeg;
    *out_len = produced;
    return ESP_OK;
}

/*
 * One photo, start to finish, and the sensor stopped again whatever
 * happens: its DMA buffers (internal RAM) and frame buffers (PSRAM) exist
 * only for the second this takes. The pixels are copied out before the
 * sensor stops and encoded afterwards, so the encoder's memory and the
 * sensor's are never needed at once. The sensor's exposure settles over its
 * first frames, so a few are skipped. A start that fails for memory is tried
 * again shortly: internal RAM frees up as the network's work comes and goes.
 */
#define START_TRIES 3
#define START_RETRY_MS 150

static esp_err_t start_with_retries(void)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < START_TRIES; i++) {
        err = sensor_start(s_jpeg != JPEG_ENCODED);
        if (err == ESP_ERR_NOT_SUPPORTED && s_jpeg == JPEG_UNKNOWN) {
            /* The sensor was found but can't make JPEG: take raw pixels instead. */
            esp_camera_deinit();
            s_jpeg = JPEG_ENCODED;
            err = sensor_start(false);
        }
        if (err == ESP_OK) {
            if (s_jpeg == JPEG_UNKNOWN) {
                s_jpeg = JPEG_NATIVE;
            }
            return ESP_OK;
        }
        if (err == ESP_ERR_CAMERA_NOT_DETECTED) {
            return ESP_ERR_TIMEOUT;   /* no sensor answered: no point retrying */
        }
        vTaskDelay(pdMS_TO_TICKS(START_RETRY_MS));
    }
    return err == ESP_ERR_NO_MEM || err == ESP_FAIL ? ESP_ERR_NO_MEM : err;   /* esp32-camera says ESP_FAIL for a failed DMA malloc */
}

static esp_err_t capture(camera_frame_t *out)
{
    int64_t started = esp_timer_get_time();
    esp_err_t err = start_with_retries();
    if (err != ESP_OK) {
        return err;
    }
    camera_fb_t *fb = NULL;
    for (int i = 0; i <= WARMUP_FRAMES; i++) {
        if (fb) {
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(WARMUP_DELAY_MS));
        }
        fb = esp_camera_fb_get();
        if (!fb) {
            break;
        }
    }
    uint8_t *pixels = NULL;
    size_t len = 0;
    int width = 0, height = 0;
    bool native = false;
    if (fb && fb->len) {
        pixels = copy_of(fb->buf, fb->len);
        len = fb->len;
        width = fb->width;
        height = fb->height;
        native = fb->format == PIXFORMAT_JPEG;
    }
    if (fb) {
        esp_camera_fb_return(fb);
    }
    esp_camera_deinit();   /* the sensor's memory goes back before the encoder's is taken */
    int64_t grabbed = esp_timer_get_time();
    if (!fb || !len) {
        ESP_LOGW(TAG, "no frame from the sensor");
        return ESP_FAIL;
    }
    if (!pixels) {
        return ESP_ERR_NO_MEM;
    }
    frame_priv_t *priv = calloc(1, sizeof(*priv));
    if (!priv) {
        jpeg_free_align(pixels);
        return ESP_ERR_NO_MEM;
    }
    size_t jpeg_len = 0;
    if (native) {
        priv->jpeg = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (priv->jpeg) {
            memcpy(priv->jpeg, pixels, len);
            jpeg_len = len;
        }
        err = priv->jpeg ? ESP_OK : ESP_ERR_NO_MEM;
    } else {
        err = encode(pixels, len, width, height, &priv->jpeg, &jpeg_len);
    }
    jpeg_free_align(pixels);
    if (err != ESP_OK) {
        free(priv);
        return err;
    }
    ESP_LOGI(TAG, "%dx%d: sensor %lld ms, encode %lld ms, %u bytes", width, height,
             (long long)((grabbed - started) / 1000), (long long)((esp_timer_get_time() - grabbed) / 1000), (unsigned)jpeg_len);
    out->jpeg = priv->jpeg;
    out->len = jpeg_len;
    out->width = width;
    out->height = height;
    out->priv = priv;
    return ESP_OK;
}

static void release(camera_frame_t *frame)
{
    frame_priv_t *priv = frame->priv;
    free(priv->jpeg);
    free(priv);
}

static camera_driver_t s_driver = {
    .max_width = 640,
    .max_height = 480,
    .capture = capture,
    .release = release,
};

const camera_driver_t *camera_dvp(const camera_dvp_config_t *config)
{
    s_cfg = *config;
    s_driver.name = s_cfg.name;
    return &s_driver;
}
