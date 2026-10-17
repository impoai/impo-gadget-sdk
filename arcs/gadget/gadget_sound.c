/*
 * SDK speaker: one sound at a time, played from a PSRAM ring of 16 kHz mono
 * PCM by an output task. A producer (beep, TTS stream) calls
 * gadget_sound_begin(), writes PCM, then gadget_sound_end(). A new sound or
 * gadget_sound_stop() drops whatever is still queued. The board's amplifier
 * is switched on with the first samples and off after a second of silence.
 */
#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "lisa_kv.h"

#include "platform_arcs.h"
#include "gadget_board.h"
#include "gadget_pcm.h"
#include "gadget_sound.h"

#define TAG "gadget_sound"
#include "lisa_log.h"

#define RATE            GADGET_SOUND_RATE
#define RING_SAMPLES    (RATE * 30)     /* 30 s, 960 KB of PSRAM: a long reply never stalls the link */
#define BLOCK           256             /* one DMA buffer, 16 ms */
#define AMP_OFF_MS      1000
#define AMP_ON_SETTLE_MS 20
#define KV_VOLUME       "gadget.volume"
#define DEFAULT_VOLUME  70

static int16_t *s_ring;
static size_t s_head, s_tail, s_count;     /* guarded by s_lock */
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile uint32_t s_gen;            /* bumped by begin/stop: older producers give up */
static volatile gadget_sound_state_t s_state = GADGET_SOUND_IDLE;
static volatile bool s_input_done = true;
static char s_error[48];
static int s_volume = DEFAULT_VOLUME;
static int32_t s_gain_q15;
static void (*s_set_amp)(bool on);

static void apply_volume(int percent)
{
    s_volume = percent;
    /* Squared curve: loudness feels closer to linear in percent. */
    s_gain_q15 = (int32_t)percent * percent * 32767 / 10000;
}

/* ---- ring ---- */

static size_t ring_take(int16_t *out, size_t max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_count < max ? s_count : max;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_ring[s_tail];
        s_tail = (s_tail + 1) % RING_SAMPLES;
    }
    s_count -= n;
    xSemaphoreGive(s_lock);
    return n;
}

static void ring_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_head = s_tail = s_count = 0;
    xSemaphoreGive(s_lock);
}

size_t gadget_sound_room(void)
{
    return RING_SAMPLES - s_count;
}

/* ---- producer side ---- */

uint32_t gadget_sound_begin(const char *what)
{
    ring_clear();
    s_gen++;
    s_input_done = false;
    s_error[0] = '\0';
    s_state = GADGET_SOUND_FETCHING;
    LISA_LOGI(TAG, "begin %s", what);
    return s_gen;
}

int gadget_sound_write(uint32_t gen, const int16_t *pcm, size_t samples, uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (samples > 0) {
        if (gen != s_gen) {
            return -1;   /* replaced or stopped */
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t room = RING_SAMPLES - s_count;
        size_t n = samples < room ? samples : room;
        for (size_t i = 0; i < n; i++) {
            s_ring[s_head] = pcm[i];
            s_head = (s_head + 1) % RING_SAMPLES;
        }
        s_count += n;
        xSemaphoreGive(s_lock);
        if (n > 0 && (s_state == GADGET_SOUND_FETCHING || s_state == GADGET_SOUND_DECODING)) {
            s_state = GADGET_SOUND_PLAYING;
        }
        pcm += n;
        samples -= n;
        if (samples > 0) {
            if (waited >= timeout_ms) {
                return -2;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            waited += 20;
        }
    }
    return 0;
}

void gadget_sound_end(uint32_t gen, const char *error)
{
    if (gen != s_gen) {
        return;
    }
    s_input_done = true;
    if (error != NULL) {
        snprintf(s_error, sizeof(s_error), "%s", error);
        if (s_count == 0) {
            s_state = GADGET_SOUND_FAILED;
        }
        LISA_LOGW(TAG, "sound failed: %s", error);
    }
}

void gadget_sound_set_decoding(uint32_t gen)
{
    if (gen == s_gen && s_state == GADGET_SOUND_FETCHING) {
        s_state = GADGET_SOUND_DECODING;
    }
}

void gadget_sound_stop(void)
{
    s_gen++;
    ring_clear();
    s_input_done = true;
    s_state = GADGET_SOUND_IDLE;
}

/* ---- state ---- */

gadget_sound_state_t gadget_sound_state(void)
{
    return s_state;
}

const char *gadget_sound_error(void)
{
    return s_error;
}

int gadget_sound_volume(void)
{
    return s_volume;
}

int gadget_sound_set_volume(int percent)
{
    if (percent < 0 || percent > 100) {
        return -1;
    }
    apply_volume(percent);
    lisa_kv_set_int(KV_VOLUME, percent);
    return 0;
}

/* ---- beep: a short rising chirp, 620 -> 1180 Hz over 90 ms (the impo-gadget-sdk sound) ---- */

static int chirp(double f0, double f1)
{
    enum { N = RATE * 90 / 1000 };
    static int16_t *pcm;
    if (pcm == NULL && (pcm = malloc(N * sizeof(int16_t))) == NULL) {
        return -1;
    }
    double phase = 0;
    for (int i = 0; i < N; i++) {
        double x = (double)i / N;
        double f = f0 + (f1 - f0) * x;
        phase += 2 * M_PI * f / RATE;
        double env = (x < 0.1 ? x / 0.1 : 1.0) * (1 - x) * (1 - x);
        double v = sin(phase) + 0.3 * sin(3 * phase);
        pcm[i] = (int16_t)(7000 * env * v);
    }
    uint32_t gen = gadget_sound_begin("beep");
    int r = gadget_sound_write(gen, pcm, N, 500);
    gadget_sound_end(gen, NULL);
    return r;
}

int gadget_sound_beep(void)
{
    return chirp(620, 1180);
}

int gadget_sound_beep_down(void)
{
    return chirp(1180, 620);
}

/* ---- output task ---- */

static void out_task(void *arg)
{
    (void)arg;
    static int16_t block[BLOCK];
    int wd = plat_watchdog_register("gadget_snd", 5000);
    bool amp = false;
    uint32_t quiet_ms = 0;
    uint16_t tail_rms[10] = { 0 }, tail_peak[10] = { 0 };   /* the last 160 ms played, for the log */
    unsigned tail_at = 0;
    bool played = false;
    for (;;) {
        plat_watchdog_checkin(wd);
        size_t n = ring_take(block, BLOCK);
        if (n == 0) {
            if (played) {
                played = false;
                char line[160];
                int o = 0;
                for (unsigned i = 0; i < 10; i++) {
                    unsigned k = (tail_at + i) % 10;
                    o += snprintf(line + o, sizeof(line) - o, " %u/%u", tail_rms[k], tail_peak[k]);
                }
                LISA_LOGI(TAG, "ended; last 10 blocks rms/peak:%s", line);
            }
            if (s_input_done && s_state == GADGET_SOUND_PLAYING) {
                s_state = GADGET_SOUND_IDLE;
            }
            if (amp && quiet_ms >= AMP_OFF_MS) {
                LISA_LOGI(TAG, "amp off");
                s_set_amp(false);
                amp = false;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            quiet_ms += 10;
            continue;
        }
        quiet_ms = 0;
        if (!amp && s_set_amp != NULL) {
            s_set_amp(true);
            amp = true;
            vTaskDelay(pdMS_TO_TICKS(AMP_ON_SETTLE_MS));
        }
        for (size_t i = 0; i < n; i++) {
            block[i] = (int16_t)(((int32_t)block[i] * s_gain_q15) >> 15);
        }
        if (n < BLOCK) {
            memset(block + n, 0, (BLOCK - n) * sizeof(int16_t));
        }
        int peak = 0;
        for (size_t i = 0; i < BLOCK; i++) {
            int a = block[i] < 0 ? -block[i] : block[i];
            peak = a > peak ? a : peak;
        }
        tail_rms[tail_at] = (uint16_t)gadget_pcm_rms(block, BLOCK);
        tail_peak[tail_at] = (uint16_t)peak;
        tail_at = (tail_at + 1) % 10;
        played = true;
        plat_audio_write(block, BLOCK);
    }
}

int gadget_sound_start(void (*set_amp)(bool on))
{
    s_set_amp = set_amp;
    int v = DEFAULT_VOLUME;
    if (lisa_kv_get_int(KV_VOLUME, &v) != 0 || v < 0 || v > 100) {
        v = DEFAULT_VOLUME;
    }
    apply_volume(v);
    s_ring = malloc(RING_SAMPLES * sizeof(int16_t));
    s_lock = xSemaphoreCreateMutex();
    if (s_ring == NULL || s_lock == NULL) {
        LISA_LOGE(TAG, "no memory for the sound ring");
        return -1;
    }
    if (plat_audio_start() != 0) {
        return -1;
    }
    if (xTaskCreate(out_task, "gadget_snd", 3072 / sizeof(StackType_t), NULL, 4, &s_task) != pdPASS) {
        return -1;
    }
    LISA_LOGI(TAG, "speaker ready, volume %d%%", s_volume);
    return 0;
}
