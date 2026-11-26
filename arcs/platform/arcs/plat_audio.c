/*
 * Speaker output through the on-chip DAC (audio0): 16 kHz, mono, 16-bit.
 * The stream starts once and stays started; when nothing is written the DAC
 * plays silence. Playback is CP-only, the AP core is not involved.
 * Settings from mtt1.0-impo V2.1 (app_wakeup.c start_playback, prj.conf gains).
 */
#include <errno.h>
#include "FreeRTOS.h"
#include "semphr.h"

#include "lisa_audio.h"
#include "lisa_device.h"

#include "platform_arcs.h"

#define TAG "plat_audio"
#include "lisa_log.h"

#define BUFFER_COUNT     12
#define BUFFER_SAMPLES   256
#define ANALOG_GAIN_DB   0
#define DIGITAL_GAIN_DB  (-2)
#define MIC_ANALOG_DB    36      /* mtt1.0-impo CONFIG_AUDIO_DEFAULT_MIC_GAIN_A_DB */
#define MIC_DIGITAL_DB   0

static lisa_device_t *s_dev;
static SemaphoreHandle_t s_mic_lock;

int plat_audio_start(void)
{
    if (s_dev != NULL) {
        return 0;
    }
    lisa_device_t *dev = lisa_device_get("audio0");
    if (!lisa_device_ready(dev)) {
        LISA_LOGE(TAG, "audio0 not ready");
        return -ENODEV;
    }
    lisa_audio_play_config_t cfg = {
        .format = {
            .sample_rate = LISA_AUDIO_RATE_16K,
            .channels = LISA_AUDIO_CH_LEFT,
            .sample_bits = LISA_AUDIO_BIT_16,
        },
        .gain = { .analog_gain = ANALOG_GAIN_DB, .digital_gain = DIGITAL_GAIN_DB },
        .buffer_count = BUFFER_COUNT,
        .buffer_samples = BUFFER_SAMPLES,
    };
    int r = lisa_audio_play_config(dev, &cfg);
    if (r == LISA_DEVICE_OK) {
        r = lisa_audio_play_start(dev);
    }
    if (r != LISA_DEVICE_OK) {
        LISA_LOGE(TAG, "play start failed: %d", r);
        return r;
    }
    s_mic_lock = xSemaphoreCreateMutex();
    if (!s_mic_lock) return -ENOMEM;
    s_dev = dev;
    LISA_LOGI(TAG, "speaker stream up: 16 kHz mono");
    return 0;
}

static plat_audio_mic_cb_t volatile s_mic_cb, s_wake_cb;
static bool s_recording, s_record_stereo;

static void on_audio(const lisa_audio_event_t *ev, void *user)
{
    (void)user;
    plat_audio_mic_cb_t cb = s_mic_cb ? s_mic_cb : s_wake_cb;
    if (cb != NULL && ev->record_buffer != NULL && ev->record_samples > 0) {
        cb((const int16_t *)ev->record_buffer, ev->record_samples);
    }
}

static bool s_mic_stereo;

void plat_audio_mic_stereo(bool on)
{
    s_mic_stereo = on;
}

static int record_start(void)
{
    if (s_dev == NULL) {
        return -ENODEV;
    }
    lisa_audio_record_config_t cfg = {
        .format = {
            .sample_rate = LISA_AUDIO_RATE_16K,
            .channels = s_mic_stereo ? LISA_AUDIO_CH_STEREO : LISA_AUDIO_CH_LEFT,   /* left: the mic */
            .sample_bits = LISA_AUDIO_BIT_16,
        },
        .gain = { .analog_gain = MIC_ANALOG_DB, .digital_gain = MIC_DIGITAL_DB },
        .differential_input = true,
        .enable_hpf = true,
    };
    static bool registered;
    if (!registered) {
        if (lisa_audio_register_callback(s_dev, on_audio, NULL) != LISA_DEVICE_OK) {
            return -EIO;
        }
        registered = true;
    }
    int r = lisa_audio_record_config(s_dev, &cfg);
    if (r == LISA_DEVICE_OK) {
        r = lisa_audio_record_start(s_dev);
    }
    if (r != LISA_DEVICE_OK) {
        LISA_LOGE(TAG, "mic start failed: %d", r);
        return -EIO;
    }
    s_recording = true;
    s_record_stereo = s_mic_stereo;
    return 0;
}

int plat_audio_mic_start(plat_audio_mic_cb_t cb)
{
    if (!s_dev || !s_mic_lock || !cb) return -ENODEV;
    xSemaphoreTake(s_mic_lock, portMAX_DELAY);
    int r = s_mic_cb || (s_recording && s_record_stereo != s_mic_stereo) ? -EBUSY : 0;
    if (!r && !s_recording) r = record_start();
    if (!r) s_mic_cb = cb;
    xSemaphoreGive(s_mic_lock);
    return r;
}
static void stop_unused(void)
{
    if (s_recording && !s_mic_cb && !s_wake_cb) {
        lisa_audio_record_stop(s_dev);
        s_recording = false;
    }
}
void plat_audio_mic_stop(void)
{
    if (!s_mic_lock) return;
    xSemaphoreTake(s_mic_lock, portMAX_DELAY);
    s_mic_cb = NULL;
    stop_unused();
    xSemaphoreGive(s_mic_lock);
}
int plat_audio_wake_start(plat_audio_mic_cb_t cb)
{
    if (!s_dev || !s_mic_lock || !cb) return -ENODEV;
    xSemaphoreTake(s_mic_lock, portMAX_DELAY);
    int r = s_wake_cb || s_mic_stereo || (s_recording && s_record_stereo) ? -EBUSY : 0;
    if (!r && !s_recording) r = record_start();
    if (!r) s_wake_cb = cb;
    xSemaphoreGive(s_mic_lock);
    return r;
}
void plat_audio_wake_stop(void)
{
    if (!s_mic_lock) return;
    xSemaphoreTake(s_mic_lock, portMAX_DELAY);
    s_wake_cb = NULL;
    stop_unused();
    xSemaphoreGive(s_mic_lock);
}

int plat_audio_write(const int16_t *pcm, size_t samples)
{
    if (s_dev == NULL) {
        return -ENODEV;
    }
    /* Returns the samples queued; blocks while the DMA buffers are full. */
    return lisa_audio_play_write(s_dev, pcm, (uint32_t)samples) == (int)samples ? 0 : -EIO;
}
