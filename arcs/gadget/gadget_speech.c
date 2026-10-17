/*
 * speaker.say: asks Impo for speech (POST /tts on the link, a 24 kHz WAV) and
 * plays it while it downloads. The WAV is decoded as it streams in
 * (gadget_pcm) and written to gadget_sound at the speaker's rate.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "gadget_pcm.h"
#include "gadget_sound.h"
#include "gadget_speech.h"
#include "impo_link.h"

#define TAG "gadget_speech"
#include "lisa_log.h"

#define WRITE_WAIT_MS 5000

static struct {
    uint32_t gen;
    bool failed;
    uint32_t bytes;
    gadget_wav_t wav;
} s;

static int emit(void *ctx, const int16_t *pcm, size_t n)
{
    (void)ctx;
    return gadget_sound_write(s.gen, pcm, n, WRITE_WAIT_MS);
}

static void fail(const char *why)
{
    if (!s.failed) {
        s.failed = true;
        gadget_sound_end(s.gen, why);
    }
}

/* ---- link stream sink, on the link task ---- */

static void on_begin(void)
{
    memset(&s, 0, sizeof(s));
    s.gen = gadget_sound_begin("say");
    gadget_wav_init(&s.wav, GADGET_SOUND_RATE, emit, NULL);
}

static void on_status(int status)
{
    if (status != 200) {
        LISA_LOGW(TAG, "/tts status %d", status);
        fail("no speech came back");
    }
}

static void on_data(const unsigned char *data, size_t len)
{
    if (s.failed) {
        return;
    }
    bool was_in = s.wav.in_data;
    s.bytes += len;
    gadget_wav_result_t r = gadget_wav_feed(&s.wav, data, len);
    if (!was_in && s.wav.in_data) {
        LISA_LOGI(TAG, "speech: %u Hz, %u channel(s)", (unsigned)s.wav.rate, (unsigned)s.wav.channels);
    }
    if (r == GADGET_WAV_NOT_WAV) {
        fail("speech was not 16-bit WAV");
    } else if (r == GADGET_WAV_STOPPED) {
        fail("the speaker could not keep up");
    }
}

static void on_end(const char *error)
{
    if (s.failed) {
        return;
    }
    if (error != NULL) {
        fail(error);
    } else if (!s.wav.in_data) {
        fail("no speech came back");
    } else {
        LISA_LOGI(TAG, "speech downloaded: %u bytes, data chunk says %u, got %u", (unsigned)s.bytes,
                  (unsigned)s.wav.data_size, (unsigned)s.wav.pcm_bytes);
        gadget_sound_end(s.gen, NULL);
    }
}

static const impo_link_stream_sink_t s_sink = { on_begin, on_status, on_data, on_end };

int gadget_speech_say(const char *text)
{
    static int handle = -1;
    impo_link_stream_cancel(handle);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "text", text);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (json == NULL) {
        return -1;
    }
    handle = impo_link_stream_open("/tts", "application/json", "audio/wav", json, strlen(json), false, &s_sink);
    cJSON_free(json);
    return handle >= 0 ? 0 : -1;
}
