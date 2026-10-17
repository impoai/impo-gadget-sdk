/*
 * speaker.play_url (CAPABILITIES.md): downloads a public https MP3 (or WAV),
 * at most 1.5 MB, then decodes it into gadget_sound. The command returns at
 * once; speaker.status shows fetching, decoding, playing, failed.
 * MP3 decoding is the SDK's Helix decoder (modules/mp3dec).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "mp3dec.h"
#include "http_client.h"
#include "gadget_pcm.h"
#include "gadget_play_url.h"
#include "gadget_sound.h"

#define TAG "gadget_play"
#include "lisa_log.h"

#define MAX_BYTES      (1536 * 1024)
#define MAX_REDIRECTS  3
#define URL_MAX        512
#define WRITE_WAIT_MS  20000

static volatile bool s_busy;
static char s_url[URL_MAX];
static char s_last_url[URL_MAX];
static uint32_t s_gen;

static int emit(void *ctx, const int16_t *pcm, size_t n)
{
    (void)ctx;
    return gadget_sound_write(s_gen, pcm, n, WRITE_WAIT_MS);
}

/* Decodes a whole MP3 in memory. Returns NULL or why it failed. */
static const char *play_mp3(uint8_t *data, size_t len)
{
    HMP3Decoder dec = MP3InitDecoder();
    if (dec == NULL) {
        return "out of memory for the decoder";
    }
    short *pcm = malloc(1152 * 2 * sizeof(short));   /* PSRAM: internal RAM is tight */
    if (pcm == NULL) {
        MP3FreeDecoder(dec);
        return "out of memory";
    }
    gadget_pcm_conv_t conv;
    bool started = false;
    unsigned char *p = data;
    int left = (int)len;
    uint32_t frames = 0;
    const char *err = NULL;
    while (left > 0) {
        int off = MP3FindSyncWord(p, left);
        if (off < 0) {
            break;
        }
        p += off;
        left -= off;
        int r = MP3Decode(dec, &p, &left, pcm, 0);
        if (r != 0) {
            /* A bad frame: skip a byte and look for the next sync word. */
            if (left > 0) {
                p++;
                left--;
            }
            continue;
        }
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(dec, &fi);
        if (!started) {
            if (fi.nChans < 1 || fi.nChans > 2 || fi.samprate <= 0) {
                err = "unsupported MP3";
                break;
            }
            gadget_pcm_conv_init(&conv, (uint32_t)fi.samprate, (uint16_t)fi.nChans, GADGET_SOUND_RATE, emit, NULL);
            LISA_LOGI(TAG, "MP3: %d Hz, %d channel(s), %d kbps", fi.samprate, fi.nChans, fi.bitrate / 1000);
            started = true;
        }
        frames++;
        if (gadget_pcm_conv_feed(&conv, pcm, (size_t)(fi.outputSamps / fi.nChans)) != 0) {
            err = frames > 0 ? NULL : "stopped";   /* replaced or stopped: not a failure */
            break;
        }
    }
    MP3FreeDecoder(dec);
    free(pcm);
    if (err == NULL && frames == 0) {
        err = "no MP3 audio found";
    }
    LISA_LOGI(TAG, "decoded %u frames", (unsigned)frames);
    return err;
}

static const char *play_wav(const uint8_t *data, size_t len)
{
    gadget_wav_t w;
    gadget_wav_init(&w, GADGET_SOUND_RATE, emit, NULL);
    gadget_wav_result_t r = gadget_wav_feed(&w, data, len);
    return r == GADGET_WAV_NOT_WAV ? "not a 16-bit WAV" : NULL;
}

static void play_task(void *arg)
{
    (void)arg;
    char url[URL_MAX];
    strcpy(url, s_url);
    http_response_t resp;
    const char *err = NULL;
    int hops = 0;
    for (;;) {
        if (http_request("GET", url, "Accept: audio/mpeg, audio/wav, */*\r\n", NULL, 0, MAX_BYTES, &resp) != 0) {
            err = "could not download the file";
            break;
        }
        if (resp.status >= 300 && resp.status < 400 && resp.location[0] != '\0' && hops < MAX_REDIRECTS &&
            strncmp(resp.location, "https://", 8) == 0) {
            LISA_LOGI(TAG, "redirect %d -> %s", resp.status, resp.location);
            strcpy(url, resp.location);
            http_response_free(&resp);
            hops++;
            continue;
        }
        if (resp.status != 200) {
            LISA_LOGW(TAG, "GET status %d", resp.status);
            err = "the server refused the file";
        } else if (resp.truncated) {
            err = "the file is larger than 1.5 MB";
        }
        break;
    }
    if (err == NULL) {
        LISA_LOGI(TAG, "downloaded %u bytes", (unsigned)resp.body_len);
        gadget_sound_set_decoding(s_gen);
        const uint8_t *d = (const uint8_t *)resp.body;
        bool wav = resp.body_len >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WAVE", 4) == 0;
        err = wav ? play_wav(d, resp.body_len) : play_mp3((uint8_t *)resp.body, resp.body_len);
    }
    if (strcmp(err ? err : "", "could not download the file") != 0) {
        http_response_free(&resp);
    }
    gadget_sound_end(s_gen, err);
    s_busy = false;
    vTaskDelete(NULL);
}

int gadget_play_url(const char *url, const char **why)
{
    if (url == NULL || strncmp(url, "https://", 8) != 0 || strlen(url) >= URL_MAX) {
        *why = "url must be https:// and at most 511 characters";
        return -1;
    }
    if (s_busy) {
        *why = "busy: another file is still downloading";
        return -1;
    }
    s_busy = true;
    strcpy(s_url, url);
    strcpy(s_last_url, url);
    s_gen = gadget_sound_begin("play_url");
    if (xTaskCreate(play_task, "gadget_play", 8192 / sizeof(StackType_t), NULL, 3, NULL) != pdPASS) {
        s_busy = false;
        gadget_sound_end(s_gen, "out of memory");
        *why = "out of memory";
        return -1;
    }
    return 0;
}

const char *gadget_play_url_last(void)
{
    return s_last_url;
}
