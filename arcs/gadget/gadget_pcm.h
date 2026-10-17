/*
 * Audio format helpers with no RTOS or hardware dependency (host-tested):
 * a streaming converter from any-rate interleaved 16-bit PCM to mono at the
 * speaker's rate, and a streaming WAV reader on top of it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Receives converted samples; returns 0 to go on, anything else to stop. */
typedef int (*gadget_pcm_emit_t)(void *ctx, const int16_t *pcm, size_t samples);

/* ---- mono + linear resampling ---- */

typedef struct {
    uint32_t in_rate, out_rate;
    uint16_t channels;
    uint32_t step_q16;      /* input samples per output sample, Q16 */
    uint32_t pos_q16;       /* position between prev and the next input sample */
    int16_t prev;
    bool have_prev;
    gadget_pcm_emit_t emit;
    void *ctx;
} gadget_pcm_conv_t;

/* channels 1 or 2. */
void gadget_pcm_conv_init(gadget_pcm_conv_t *c, uint32_t in_rate, uint16_t channels, uint32_t out_rate,
                          gadget_pcm_emit_t emit, void *ctx);
/* frames of `channels` interleaved samples. Returns 0, or the emit's nonzero value. */
int gadget_pcm_conv_feed(gadget_pcm_conv_t *c, const int16_t *frames, size_t nframes);

/* ---- streaming WAV (RIFF, 16-bit PCM) ---- */

#define GADGET_WAV_HEAD_MAX 512

typedef enum {
    GADGET_WAV_OK = 0,
    GADGET_WAV_NOT_WAV = -1,      /* not RIFF/WAVE, or not 16-bit PCM, or more than 2 channels */
    GADGET_WAV_STOPPED = -2,      /* the emit asked to stop */
} gadget_wav_result_t;

typedef struct {
    bool in_data;
    uint8_t head[GADGET_WAV_HEAD_MAX];
    size_t head_len;
    uint32_t rate;
    uint16_t channels;
    uint32_t data_size;         /* what the data chunk header declares */
    uint32_t pcm_bytes;         /* what came after it (only data_size of it is played) */
    uint8_t carry[4];           /* a frame split across feeds */
    size_t carry_len;
    uint32_t out_rate;
    gadget_pcm_conv_t conv;
    gadget_pcm_emit_t emit;
    void *ctx;
} gadget_wav_t;

void gadget_wav_init(gadget_wav_t *w, uint32_t out_rate, gadget_pcm_emit_t emit, void *ctx);
/* Any chunking. Returns GADGET_WAV_OK, or an error that sticks. */
gadget_wav_result_t gadget_wav_feed(gadget_wav_t *w, const uint8_t *data, size_t len);

/* ---- base64 (RFC 4648, with padding) ---- */

/* out needs 4 * ((len + 2) / 3) bytes; returns the length written (no NUL). */
size_t gadget_base64(const uint8_t *in, size_t len, char *out);

/* Root mean square of n samples. */
uint32_t gadget_pcm_rms(const int16_t *pcm, size_t n);

#ifdef __cplusplus
}
#endif
