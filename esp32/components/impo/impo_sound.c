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
#include "impo_sound.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "minimp3.h"

#include "impo_audio.h"

static const char *TAG = "impo_sound";

#define OUT_RATE IMPO_AUDIO_RATE
/* minimp3 only takes a frame once it can see the next one's header. */
#define MP3_HOLD (1441 + 4)
#define IN_CHUNK 4096                     /* bytes taken from the stream at a time */
#define DECODER_STACK (32 * 1024)         /* minimp3 wants ~16 KB */

typedef struct {
    uint32_t step, pos;
    int16_t prev;
} resampler_t;

static StreamBufferHandle_t s_in;         /* bytes of the sound, as they arrive */
static StreamBufferHandle_t s_out;        /* 16 kHz mono, for the voice loop */
static TaskHandle_t s_decoder;
static mp3dec_t *s_dec;                   /* 6.5 KB, in PSRAM */
static uint8_t *s_buf;                    /* IN_CHUNK + MP3_HOLD of input being decoded */
static int16_t *s_pcm;                    /* a decoded frame */
static int16_t *s_pcm16;                  /* and it resampled */

static atomic_uint s_gen;                 /* the current sound; bytes of an older one are dropped */
static atomic_bool s_ended;               /* the producer is done with the current sound */
static atomic_bool s_ok;                  /* ... and it was whole */
static atomic_bool s_decoding;            /* the decoder is working on the current sound */
static atomic_bool s_playing;             /* PCM has gone out and not run dry since */
static char s_state[12] = "idle";
static char s_error[64];
static char s_source[8];

static void resampler_init(resampler_t *r, int in_rate, int out_rate)
{
    r->step = (uint32_t)(((uint64_t)in_rate << 16) / out_rate);
    r->pos = 0;
    r->prev = 0;
}

/* Linear interpolation; state carries across calls. out must hold n*out/in + 2. */
static size_t resample(resampler_t *r, const int16_t *in, size_t n, int16_t *out)
{
    size_t o = 0;
    if (!n) {
        return 0;
    }
    while ((r->pos >> 16) < n) {
        size_t i = r->pos >> 16;
        int32_t a = i ? in[i - 1] : r->prev;
        int32_t b = in[i];
        out[o++] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * (int64_t)(r->pos & 0xffff)) >> 16));
        r->pos += r->step;
    }
    r->pos -= n << 16;
    r->prev = in[n - 1];
    return o;
}

static void set_state(const char *state, const char *error)
{
    strlcpy(s_state, state, sizeof(s_state));
    strlcpy(s_error, error ? error : "", sizeof(s_error));
}

/* Mono 16 kHz frames to the voice loop, waiting for room; false once the sound changed. */
static bool put(unsigned gen, const int16_t *pcm, size_t frames)
{
    size_t off = 0;
    while (off < frames) {
        if (atomic_load(&s_gen) != gen) {
            return false;
        }
        size_t n = xStreamBufferSend(s_out, pcm + off, (frames - off) * sizeof(int16_t), pdMS_TO_TICKS(100)) / sizeof(int16_t);
        off += n;
        if (n) {
            atomic_store(&s_playing, true);
        }
    }
    return true;
}

/* A WAV header's rate, channels and where the samples start; false until enough has arrived. */
static bool wav_header(const uint8_t *data, size_t len, int *rate, int *channels, size_t *skip)
{
    if (len < 12 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4)) {
        return false;
    }
    size_t off = 12;
    bool fmt = false;
    while (off + 8 <= len) {
        uint32_t size = data[off + 4] | data[off + 5] << 8 | data[off + 6] << 16 | (uint32_t)data[off + 7] << 24;
        if (!memcmp(data + off, "fmt ", 4)) {
            if (off + 8 + 16 > len) {
                return false;
            }
            int tag = data[off + 8] | data[off + 9] << 8;
            *channels = data[off + 10] | data[off + 11] << 8;
            *rate = data[off + 12] | data[off + 13] << 8 | data[off + 14] << 16 | (uint32_t)data[off + 15] << 24;
            int bits = data[off + 22] | data[off + 23] << 8;
            fmt = tag == 1 && bits == 16 && (*channels == 1 || *channels == 2) && *rate >= 8000 && *rate <= 48000;
        } else if (!memcmp(data + off, "data", 4)) {
            *skip = off + 8;
            return fmt;
        }
        off += 8 + size + (size & 1);
    }
    return false;
}

/*
 * One sound: bytes from s_in until the producer ends it, decoded as they
 * come. s_buf holds what the decoder hasn't consumed yet (an MP3 frame is
 * taken only once the next header is in sight), topped up from the stream.
 */
static void decode_one(unsigned gen)
{
    enum { F_UNKNOWN, F_MP3, F_WAV } format = F_UNKNOWN;
    resampler_t rs;
    int rate = 0, channels = 1;
    size_t have = 0;
    bool ended = false;
    int frames = 0;
    while (atomic_load(&s_gen) == gen) {
        /* Top up. */
        if (!ended && have < IN_CHUNK + MP3_HOLD) {
            size_t n = xStreamBufferReceive(s_in, s_buf + have, IN_CHUNK + MP3_HOLD - have, pdMS_TO_TICKS(50));
            have += n;
            if (!n && atomic_load(&s_ended) && !xStreamBufferBytesAvailable(s_in)) {
                ended = true;
                if (!atomic_load(&s_ok)) {
                    return;   /* the producer gave up: so does the decoder */
                }
            }
            if (!n && !ended) {
                continue;
            }
        }
        if (format == F_UNKNOWN) {
            if (have < 12 && !ended) {
                continue;
            }
            if (have >= 4 && !memcmp(s_buf, "RIFF", 4)) {
                size_t skip = 0;
                if (!wav_header(s_buf, have, &rate, &channels, &skip)) {
                    if (ended || have >= 256) {
                        set_state("failed", "not 16-bit PCM WAV");
                        return;
                    }
                    continue;
                }
                format = F_WAV;
                resampler_init(&rs, rate, OUT_RATE);
                memmove(s_buf, s_buf + skip, have - skip);
                have -= skip;
                ESP_LOGI(TAG, "%s: WAV %d Hz, %d ch", s_source, rate, channels);
            } else {
                format = F_MP3;
                mp3dec_init(s_dec);
            }
        }
        size_t used = 0;
        if (format == F_WAV) {
            size_t frame_bytes = 2 * channels;
            size_t n = have / frame_bytes;
            if (n > MINIMP3_MAX_SAMPLES_PER_FRAME / 2) {
                n = MINIMP3_MAX_SAMPLES_PER_FRAME / 2;
            }
            for (size_t k = 0; k < n; k++) {
                const uint8_t *f = s_buf + k * frame_bytes;
                int16_t l = (int16_t)(f[0] | f[1] << 8);
                int16_t r = channels == 2 ? (int16_t)(f[2] | f[3] << 8) : l;
                s_pcm[k] = (int16_t)((l + r) / 2);
            }
            used = n * frame_bytes;
            if (n && !put(gen, s_pcm16, resample(&rs, s_pcm, n, s_pcm16))) {
                return;
            }
            frames += n;
        } else {
            /* Keep the hold back until the end, so a frame isn't cut short. */
            size_t avail = ended ? have : have > MP3_HOLD ? have - MP3_HOLD : 0;
            mp3dec_frame_info_t info = { 0 };
            int samples = avail ? mp3dec_decode_frame(s_dec, s_buf, avail, s_pcm, &info) : 0;
            used = info.frame_bytes;
            if (samples) {
                if (info.channels == 2) {
                    for (int k = 0; k < samples; k++) {
                        s_pcm[k] = (int16_t)((s_pcm[2 * k] + s_pcm[2 * k + 1]) / 2);
                    }
                }
                if (rate != info.hz) {
                    rate = info.hz;
                    resampler_init(&rs, rate, OUT_RATE);
                    ESP_LOGI(TAG, "%s: MP3 %d Hz, %d ch, %d kbps", s_source, info.hz, info.channels, info.bitrate_kbps);
                }
                if (!put(gen, s_pcm16, resample(&rs, s_pcm, samples, s_pcm16))) {
                    return;
                }
                frames++;
            } else if (!used && ended) {
                used = have;   /* trailing junk */
            }
        }
        if (used) {
            memmove(s_buf, s_buf + used, have - used);
            have -= used;
        } else if (ended && (format == F_WAV ? have < 2 * (size_t)channels : true)) {
            break;   /* all decoded */
        } else if (ended) {
            break;
        }
    }
    if (atomic_load(&s_gen) == gen) {
        if (!frames) {
            set_state("failed", format == F_MP3 ? "not an MP3 this gadget can decode" : "empty sound");
        } else {
            ESP_LOGI(TAG, "%s: decoded", s_source);
        }
    }
}

static void decoder_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        unsigned gen = atomic_load(&s_gen);
        atomic_store(&s_decoding, true);
        decode_one(gen);
        atomic_store(&s_decoding, false);
    }
}

esp_err_t impo_sound_init(void)
{
    s_in = xStreamBufferCreateWithCaps(IMPO_SOUND_IN_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_out = xStreamBufferCreateWithCaps(IMPO_SOUND_OUT_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_dec = heap_caps_malloc(sizeof(*s_dec), MALLOC_CAP_SPIRAM);
    s_buf = heap_caps_malloc(IN_CHUNK + MP3_HOLD, MALLOC_CAP_SPIRAM);
    s_pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_pcm16 = heap_caps_malloc((MINIMP3_MAX_SAMPLES_PER_FRAME * 2 + 16) * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_in || !s_out || !s_dec || !s_buf || !s_pcm || !s_pcm16) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCoreWithCaps(decoder_task, "sound", DECODER_STACK, NULL, 5, &s_decoder, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void impo_sound_begin(const char *source)
{
    if (!s_decoder) {
        return;
    }
    atomic_fetch_add(&s_gen, 1);
    atomic_store(&s_ended, false);
    atomic_store(&s_ok, false);
    atomic_store(&s_playing, false);
    strlcpy(s_source, source, sizeof(s_source));
    /* The decoder notices the new generation and gives up the old sound; then the buffers are ours. */
    while (atomic_load(&s_decoding)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    xStreamBufferReset(s_in);
    xStreamBufferReset(s_out);
    set_state("fetching", NULL);
    xTaskNotifyGive(s_decoder);
}

size_t impo_sound_room(void)
{
    return s_in ? xStreamBufferSpacesAvailable(s_in) : 0;
}

bool impo_sound_write(const uint8_t *data, size_t len, int wait_ms)
{
    unsigned gen = atomic_load(&s_gen);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(wait_ms);
    size_t off = 0;
    while (off < len) {
        if (atomic_load(&s_gen) != gen || !strcmp(s_state, "failed")) {
            return false;
        }
        TickType_t left = deadline - xTaskGetTickCount();
        if ((int32_t)left <= 0) {
            return false;
        }
        off += xStreamBufferSend(s_in, data + off, len - off, left > pdMS_TO_TICKS(50) ? pdMS_TO_TICKS(50) : left);
    }
    return true;
}

void impo_sound_end(bool ok)
{
    atomic_store(&s_ok, ok);
    atomic_store(&s_ended, true);
    if (!ok && !strcmp(s_state, "fetching")) {
        set_state("failed", "the sound did not arrive whole");
    }
}

void impo_sound_stop(void)
{
    atomic_fetch_add(&s_gen, 1);
    atomic_store(&s_ended, true);
    if (s_out) {
        xStreamBufferReset(s_out);
    }
    atomic_store(&s_playing, false);
    set_state("idle", NULL);
}

size_t impo_sound_read(int16_t *pcm, size_t frames, int wait_ms)
{
    if (!s_out) {
        return 0;
    }
    size_t n = xStreamBufferReceive(s_out, pcm, frames * sizeof(int16_t), pdMS_TO_TICKS(wait_ms)) / sizeof(int16_t);
    if (n && strcmp(s_state, "playing")) {
        set_state("playing", NULL);
    }
    if (!n && atomic_load(&s_playing) && !atomic_load(&s_decoding) && !xStreamBufferBytesAvailable(s_out)) {
        /* Ran dry with nothing more coming: done. */
        atomic_store(&s_playing, false);
        if (!strcmp(s_state, "playing")) {
            set_state("idle", NULL);
        }
    }
    return n;
}

bool impo_sound_active(void)
{
    return s_out && (atomic_load(&s_decoding) || atomic_load(&s_playing) || xStreamBufferBytesAvailable(s_out) > 0);
}

const char *impo_sound_status(char *error, size_t cap)
{
    if (error) {
        strlcpy(error, s_error, cap);
    }
    return s_state;
}

void impo_sound_fail(const char *why)
{
    set_state("failed", why);
    atomic_fetch_add(&s_gen, 1);
    atomic_store(&s_ended, true);
}
