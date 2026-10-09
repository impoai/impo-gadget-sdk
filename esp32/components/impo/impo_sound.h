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
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sounds outside a voice turn (speaker.say, speaker.play_url), streamed: a
 * producer writes the bytes of an MP3 or a WAV as they arrive, a decoder task
 * turns them into 16 kHz mono as they come, and the voice loop plays that
 * when it is free. Everything is sized once at start-up (IMPO_SOUND_IN_BYTES
 * of bytes, IMPO_SOUND_OUT_BYTES of PCM, the decoder's task), so a sound of
 * any length takes the same memory and nothing is allocated while it plays.
 * One sound at a time: beginning another stops the one before.
 */

#define IMPO_SOUND_IN_BYTES (160 * 1024)   /* MP3: a minute at 20 kbps; WAV: 3 s at 24 kHz */
#define IMPO_SOUND_OUT_BYTES (64 * 1024)   /* 2 s of 16 kHz mono */

/* Once, from impo_app_run; needs PSRAM. */
esp_err_t impo_sound_init(void);

/* A new sound from `source` ("say", "url"), ending whatever was playing. */
void impo_sound_begin(const char *source);
/* Room for more bytes right now; a producer on a task it must not block
 * (the Link session's) holds off reading its socket when this is low. */
size_t impo_sound_room(void);
/* Bytes of the sound, waiting up to wait_ms for room. False once the sound
 * was stopped or no room came: the producer should give up on it. */
bool impo_sound_write(const uint8_t *data, size_t len, int wait_ms);
/* No more bytes: the rest plays out. ok=false drops what is still queued. */
void impo_sound_end(bool ok);
/* Stops the sound now. */
void impo_sound_stop(void);

/* For the voice loop: decoded frames, up to wait_ms for some. */
size_t impo_sound_read(int16_t *pcm, size_t frames, int wait_ms);
/* Something is queued, decoding or playing. */
bool impo_sound_active(void);

/* "idle", "fetching", "playing" or "failed"; `error` gets why it failed, if it did. */
const char *impo_sound_status(char *error, size_t cap);
/* The producer's own failure, shown by impo_sound_status (e.g. "the URL could not be reached"). */
void impo_sound_fail(const char *why);

#ifdef __cplusplus
}
#endif
