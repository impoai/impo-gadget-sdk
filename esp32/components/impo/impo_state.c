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

#include "impo_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "impo_text.h"

#define HAPPY_SECS 1.6f

#define AWAKE_BIT BIT0   /* while !s_asleep */
#define NUDGE_BIT BIT1

static volatile impo_mode_t s_mode = IMPO_MODE_BOOT;
static volatile int64_t s_mode_since_us;
static volatile float s_level;
static volatile float s_progress;
static volatile int64_t s_last_poke_us;
static volatile int64_t s_happy_until_us;
static volatile bool s_asleep;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_caption[IMPO_CAPTION_MAX];
static uint32_t s_caption_version;
static SemaphoreHandle_t s_format_lock;
static EventGroupHandle_t s_wake;
static volatile int s_page_cols = 16, s_page_lines = 2;
static impo_power_t s_power = { .battery_pct = -1 };
static volatile bool s_as_if_battery;

static float secs_since(int64_t us)
{
    return (float)(esp_timer_get_time() - us) / 1e6f;
}

void impo_state_init(void)
{
    static StaticSemaphore_t lock;
    s_format_lock = xSemaphoreCreateMutexStatic(&lock);
    static StaticEventGroup_t wake;
    s_wake = xEventGroupCreateStatic(&wake);
    xEventGroupSetBits(s_wake, AWAKE_BIT);
    int64_t now = esp_timer_get_time();
    s_mode_since_us = now;
    s_last_poke_us = now;
}

void impo_state_set_mode(impo_mode_t mode)
{
    if (mode == s_mode) {
        return;
    }
    /* Shutting down wins over the voice pipeline finishing a turn. */
    if (s_mode == IMPO_MODE_OFF && mode != IMPO_MODE_IDLE) {
        return;
    }
    s_mode_since_us = esp_timer_get_time();
    s_mode = mode;
}

impo_mode_t impo_state_mode(float *secs_in_mode)
{
    if (secs_in_mode) {
        *secs_in_mode = secs_since(s_mode_since_us);
    }
    return s_mode;
}

void impo_state_set_level(float level)
{
    s_level = level < 0 ? 0 : (level > 1 ? 1 : level);
}

float impo_state_level(void)
{
    return s_level;
}

void impo_state_set_progress(float progress)
{
    s_progress = progress < 0 ? 0 : (progress > 1 ? 1 : progress);
}

float impo_state_progress(void)
{
    return s_progress;
}

void impo_state_set_caption(const char *fmt, ...)
{
    static char buf[sizeof(s_caption)];   /* too big for some callers' stacks */
    xSemaphoreTake(s_format_lock, portMAX_DELAY);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    impo_text_to_ascii(buf, sizeof(buf));   /* replies have curly quotes and dashes */

    portENTER_CRITICAL(&s_lock);
    if (strcmp(buf, s_caption) != 0) {
        memcpy(s_caption, buf, sizeof(s_caption));
        s_caption_version++;
    }
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_format_lock);
}

bool impo_state_caption(char *out, size_t out_len, uint32_t *version)
{
    bool changed = false;
    portENTER_CRITICAL(&s_lock);
    if (*version != s_caption_version) {
        strlcpy(out, s_caption, out_len);
        *version = s_caption_version;
        changed = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return changed;
}

void impo_state_set_page(int cols, int lines)
{
    s_page_cols = cols;
    s_page_lines = lines;
}

void impo_state_page(int *cols, int *lines)
{
    *cols = s_page_cols;
    *lines = s_page_lines;
}

void impo_state_set_power(const impo_power_t *power)
{
    bool was = impo_state_on_battery();
    portENTER_CRITICAL(&s_lock);
    s_power = *power;
    portEXIT_CRITICAL(&s_lock);
    if (impo_state_on_battery() != was) {
        impo_state_nudge();
    }
}

impo_power_t impo_state_power(void)
{
    portENTER_CRITICAL(&s_lock);
    impo_power_t p = s_power;
    portEXIT_CRITICAL(&s_lock);
    return p;
}

bool impo_state_on_battery(void)
{
    impo_power_t p = impo_state_power();
    return s_as_if_battery || (!p.usb && p.battery_pct >= 0);
}

void impo_state_set_as_if_battery(bool on)
{
    if (on != s_as_if_battery) {
        s_as_if_battery = on;
        impo_state_nudge();
    }
}

void impo_state_poke(void)
{
    s_last_poke_us = esp_timer_get_time();
}

float impo_state_idle_secs(void)
{
    return secs_since(s_last_poke_us);
}

void impo_state_set_asleep(bool asleep)
{
    if (!asleep) {
        impo_state_poke();
    }
    s_asleep = asleep;
    if (asleep) {
        xEventGroupClearBits(s_wake, AWAKE_BIT);
    } else {
        xEventGroupSetBits(s_wake, AWAKE_BIT);
    }
}

bool impo_state_asleep(void)
{
    return s_asleep;
}

void impo_state_wait_awake(uint32_t timeout_ms)
{
    xEventGroupWaitBits(s_wake, AWAKE_BIT | NUDGE_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    xEventGroupClearBits(s_wake, NUDGE_BIT);
}

void impo_state_nudge(void)
{
    xEventGroupSetBits(s_wake, NUDGE_BIT);
}

void impo_state_make_happy(void)
{
    s_happy_until_us = esp_timer_get_time() + (int64_t)(HAPPY_SECS * 1e6f);
    impo_state_poke();
}

float impo_state_happiness(void)
{
    float left = (float)(s_happy_until_us - esp_timer_get_time()) / 1e6f;
    if (left <= 0) {
        return 0;
    }
    /* Ease out over the last 0.4 s. */
    return left > 0.4f ? 1.0f : left / 0.4f;
}
