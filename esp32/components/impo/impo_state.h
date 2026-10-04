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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Shared state between the voice pipeline (writer) and the UI (reader).
 * Scalars are plain word-sized stores; the caption is guarded by a spinlock.
 */

typedef enum {
    IMPO_MODE_BOOT = 0,
    IMPO_MODE_IDLE,
    IMPO_MODE_LISTENING,
    IMPO_MODE_THINKING,
    IMPO_MODE_SPEAKING,
    IMPO_MODE_ERROR,
    IMPO_MODE_OFF,       /* powering down */
    IMPO_MODE_COUNT,
} impo_mode_t;

typedef struct {
    int battery_pct;   /* -1 when no battery is connected */
    int battery_mv;    /* 0 when the board can't measure it */
    bool charging;
    bool usb;
} impo_power_t;

void impo_state_init(void);

void impo_state_set_mode(impo_mode_t mode);
impo_mode_t impo_state_mode(float *secs_in_mode);

/* Live audio level (mic while listening, playback while speaking), 0..1. */
void impo_state_set_level(float level);
float impo_state_level(void);

/* Progress of the current phase (recording length or playback), 0..1. */
void impo_state_set_progress(float progress);
float impo_state_progress(void);

/* Room for a page of reply text; longer captions are cut short. */
#define IMPO_CAPTION_MAX 400

void impo_state_set_caption(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Copies the caption if it changed since *version; returns true on change. */
bool impo_state_caption(char *out, size_t out_len, uint32_t *version);

/* How much reply text the screen shows at once, set by the UI: lines of up to
 * `cols` characters. The replies are wrapped and paged to fit. */
void impo_state_set_page(int cols, int lines);
void impo_state_page(int *cols, int *lines);

void impo_state_set_power(const impo_power_t *power);
impo_power_t impo_state_power(void);
/* A battery and no USB power (charger or computer). Power saving asleep (the
 * voice task resting, the display paused, the Wi-Fi nap) is only for then;
 * a change nudges (impo_state_wait_awake). */
bool impo_state_on_battery(void);
/* Bench tests over USB (">nap"): count as on battery until turned off. */
void impo_state_set_as_if_battery(bool on);

/* Any user interaction: keeps Impo awake. */
void impo_state_poke(void);
float impo_state_idle_secs(void);

/* Screen sleep (display dark, rendering paused). Waking also pokes. */
void impo_state_set_asleep(bool asleep);
bool impo_state_asleep(void);
/* For a task with nothing to do asleep: returns once awake, nudged
 * (impo_state_nudge, which doesn't wake the screen) or timeout_ms passes. */
void impo_state_wait_awake(uint32_t timeout_ms);
void impo_state_nudge(void);

/* Touch/pet reaction. */
void impo_state_make_happy(void);
float impo_state_happiness(void);
