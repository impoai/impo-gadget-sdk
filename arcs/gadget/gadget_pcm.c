/* Audio format helpers; see gadget_pcm.h. Pure C, host-tested (tests/host/test_gadget_pcm.c). */
#include <string.h>

#include "gadget_pcm.h"

#define OUT_CHUNK 256

void gadget_pcm_conv_init(gadget_pcm_conv_t *c, uint32_t in_rate, uint16_t channels, uint32_t out_rate,
                          gadget_pcm_emit_t emit, void *ctx)
{
    memset(c, 0, sizeof(*c));
    c->in_rate = in_rate;
    c->out_rate = out_rate;
    c->channels = channels;
    c->step_q16 = (uint32_t)(((uint64_t)in_rate << 16) / out_rate);
    c->emit = emit;
    c->ctx = ctx;
}

int gadget_pcm_conv_feed(gadget_pcm_conv_t *c, const int16_t *frames, size_t nframes)
{
    int16_t out[OUT_CHUNK];
    size_t n = 0;
    for (size_t i = 0; i < nframes; i++) {
        int32_t sum = 0;
        for (uint16_t ch = 0; ch < c->channels; ch++) {
            sum += frames[i * c->channels + ch];
        }
        int16_t cur = (int16_t)(sum / c->channels);
        if (!c->have_prev) {
            c->prev = cur;
            c->have_prev = true;
            continue;
        }
        /* Output samples that fall between prev and cur. */
        while (c->pos_q16 < 65536) {
            out[n++] = (int16_t)(c->prev + (((int32_t)(cur - c->prev) * (int32_t)c->pos_q16) >> 16));
            c->pos_q16 += c->step_q16;
            if (n == OUT_CHUNK) {
                int r = c->emit(c->ctx, out, n);
                if (r != 0) {
                    return r;
                }
                n = 0;
            }
        }
        c->pos_q16 -= 65536;
        c->prev = cur;
    }
    return n > 0 ? c->emit(c->ctx, out, n) : 0;
}

/* ---- WAV ---- */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void gadget_wav_init(gadget_wav_t *w, uint32_t out_rate, gadget_pcm_emit_t emit, void *ctx)
{
    memset(w, 0, sizeof(*w));
    w->out_rate = out_rate;
    w->emit = emit;
    w->ctx = ctx;
}

/* Offset of the first PCM byte in head; 0 when more is needed; -1 when it is not usable WAV. */
static int parse_head(gadget_wav_t *w)
{
    if (w->head_len < 12) {
        return 0;
    }
    if (memcmp(w->head, "RIFF", 4) != 0 || memcmp(w->head + 8, "WAVE", 4) != 0) {
        return -1;
    }
    size_t off = 12;
    while (off + 8 <= w->head_len) {
        uint32_t size = le32(w->head + off + 4);
        if (memcmp(w->head + off, "fmt ", 4) == 0) {
            if (off + 8 + 16 > w->head_len) {
                return 0;
            }
            const uint8_t *f = w->head + off + 8;
            if (le16(f) != 1 || le16(f + 14) != 16) {
                return -1;
            }
            w->channels = le16(f + 2);
            w->rate = le32(f + 4);
            if (w->channels < 1 || w->channels > 2 || w->rate == 0) {
                return -1;
            }
        } else if (memcmp(w->head + off, "data", 4) == 0) {
            w->data_size = size;
            return w->rate != 0 ? (int)(off + 8) : -1;
        }
        off += 8 + (size_t)size + (size & 1);
    }
    return w->head_len >= GADGET_WAV_HEAD_MAX ? -1 : 0;
}

static gadget_wav_result_t feed_pcm(gadget_wav_t *w, const uint8_t *data, size_t len)
{
    size_t stride = 2u * w->channels;
    int16_t frame[2];
    /*
     * Stop at the end of the data chunk: Impo's /tts WAV has another chunk after
     * the samples (about 6 KB), and playing it as PCM is a loud buzz at the end.
     * A streaming writer may leave the size 0 or 0xFFFFFFFF; then play it all.
     */
    if (w->data_size != 0 && w->data_size != 0xFFFFFFFFu) {
        size_t left = w->pcm_bytes < w->data_size ? w->data_size - w->pcm_bytes : 0;
        w->pcm_bytes += len;
        len = len < left ? len : left;
    } else {
        w->pcm_bytes += len;
    }
    if (w->carry_len > 0) {
        size_t take = stride - w->carry_len < len ? stride - w->carry_len : len;
        memcpy(w->carry + w->carry_len, data, take);
        w->carry_len += take;
        data += take;
        len -= take;
        if (w->carry_len < stride) {
            return GADGET_WAV_OK;
        }
        frame[0] = (int16_t)le16(w->carry);
        frame[1] = (int16_t)le16(w->carry + 2);
        w->carry_len = 0;
        if (gadget_pcm_conv_feed(&w->conv, frame, 1) != 0) {
            return GADGET_WAV_STOPPED;
        }
    }
    size_t frames = len / stride;
    /* The data may be unaligned: convert in small aligned batches. */
    int16_t batch[256];
    size_t per = sizeof(batch) / sizeof(batch[0]) / w->channels;
    for (size_t f = 0; f < frames; f += per) {
        size_t k = frames - f < per ? frames - f : per;
        for (size_t i = 0; i < k * w->channels; i++) {
            batch[i] = (int16_t)le16(data + (f * w->channels + i) * 2);
        }
        if (gadget_pcm_conv_feed(&w->conv, batch, k) != 0) {
            return GADGET_WAV_STOPPED;
        }
    }
    size_t rest = len - frames * stride;
    memcpy(w->carry, data + frames * stride, rest);
    w->carry_len = rest;
    return GADGET_WAV_OK;
}

gadget_wav_result_t gadget_wav_feed(gadget_wav_t *w, const uint8_t *data, size_t len)
{
    if (w->in_data) {
        return feed_pcm(w, data, len);
    }
    size_t take = GADGET_WAV_HEAD_MAX - w->head_len < len ? GADGET_WAV_HEAD_MAX - w->head_len : len;
    memcpy(w->head + w->head_len, data, take);
    w->head_len += take;
    int at = parse_head(w);
    if (at < 0) {
        return GADGET_WAV_NOT_WAV;
    }
    if (at == 0) {
        return GADGET_WAV_OK;
    }
    w->in_data = true;
    gadget_pcm_conv_init(&w->conv, w->rate, w->channels, w->out_rate, w->emit, w->ctx);
    gadget_wav_result_t r = feed_pcm(w, w->head + at, w->head_len - (size_t)at);
    return r != GADGET_WAV_OK ? r : feed_pcm(w, data + take, len - take);
}

/* ---- base64, rms ---- */

size_t gadget_base64(const uint8_t *in, size_t len, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = i + 1 < len ? t[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? t[v & 63] : '=';
    }
    return o;
}

uint32_t gadget_pcm_rms(const int16_t *pcm, size_t n)
{
    if (n == 0) {
        return 0;
    }
    uint64_t sq = 0;
    for (size_t i = 0; i < n; i++) {
        sq += (uint64_t)((int32_t)pcm[i] * pcm[i]);
    }
    uint64_t m = sq / n;
    uint32_t r = 0;
    for (uint32_t bit = 1u << 15; bit != 0; bit >>= 1) {
        if ((uint64_t)(r | bit) * (r | bit) <= m) {
            r |= bit;
        }
    }
    return r;
}
