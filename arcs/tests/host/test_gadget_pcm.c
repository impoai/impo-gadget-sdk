/* Host tests for gadget/gadget_pcm.c: resampling, WAV streaming, base64, rms. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "host_check.h"
#include "gadget_pcm.h"

static int16_t g_out[200000];
static size_t g_n;
static int g_stop_after;   /* emit returns nonzero once this many samples arrived (0: never) */

static int collect(void *ctx, const int16_t *pcm, size_t n)
{
    (void)ctx;
    for (size_t i = 0; i < n && g_n < sizeof(g_out) / sizeof(g_out[0]); i++) {
        g_out[g_n++] = pcm[i];
    }
    return (g_stop_after && g_n >= (size_t)g_stop_after) ? 7 : 0;
}

static void reset(void)
{
    g_n = 0;
    g_stop_after = 0;
}

/* Builds a WAV of `frames` frames of a sine (same value on every channel). */
static size_t make_wav(uint8_t *out, uint32_t rate, uint16_t ch, size_t frames, bool extra_chunk)
{
    uint8_t *p = out;
    memcpy(p, "RIFF\xff\xff\xff\xffWAVE", 12);
    p += 12;
    if (extra_chunk) {   /* a LIST chunk before fmt, odd-sized to test padding */
        memcpy(p, "LIST\x03\x00\x00\x00" "abc\x00", 12);
        p += 12;
    }
    uint32_t byte_rate = rate * ch * 2;
    uint8_t fmt[24] = { 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, (uint8_t)ch, 0,
                        (uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16), 0,
                        (uint8_t)byte_rate, (uint8_t)(byte_rate >> 8), (uint8_t)(byte_rate >> 16), 0,
                        (uint8_t)(ch * 2), 0, 16, 0 };
    memcpy(p, fmt, 24);
    p += 24;
    memcpy(p, "data\xff\xff\xff\xff", 8);
    p += 8;
    for (size_t i = 0; i < frames; i++) {
        int16_t v = (int16_t)(10000 * sin(2 * M_PI * 440 * i / rate));
        for (uint16_t c = 0; c < ch; c++) {
            *p++ = (uint8_t)v;
            *p++ = (uint8_t)(v >> 8);
        }
    }
    return (size_t)(p - out);
}

static void test_resample_counts(void)
{
    gadget_pcm_conv_t c;
    static int16_t in[24000];
    for (int i = 0; i < 24000; i++) {
        in[i] = 1000;
    }
    reset();
    gadget_pcm_conv_init(&c, 24000, 1, 16000, collect, NULL);
    gadget_pcm_conv_feed(&c, in, 24000);
    CHECK(g_n >= 15990 && g_n <= 16000, "24 kHz -> 16 kHz: one second stays one second");
    CHECK(g_out[0] == 1000 && g_out[g_n - 1] == 1000, "a constant stays constant");

    reset();
    gadget_pcm_conv_init(&c, 16000, 1, 16000, collect, NULL);
    for (int i = 0; i < 16000; i++) {
        in[i] = (int16_t)i;
    }
    gadget_pcm_conv_feed(&c, in, 16000);
    CHECK(g_n == 15999 && g_out[0] == 0 && g_out[100] == 100, "16 kHz -> 16 kHz passes samples through");

    /* Feeding in pieces gives the same result as at once. */
    static int16_t once[20000];
    reset();
    gadget_pcm_conv_init(&c, 44100, 1, 16000, collect, NULL);
    for (int i = 0; i < 4410; i++) {
        in[i] = (int16_t)(i * 7);
    }
    gadget_pcm_conv_feed(&c, in, 4410);
    size_t n_once = g_n;
    memcpy(once, g_out, n_once * sizeof(int16_t));
    reset();
    gadget_pcm_conv_init(&c, 44100, 1, 16000, collect, NULL);
    for (int i = 0; i < 4410; i += 37) {
        gadget_pcm_conv_feed(&c, in + i, (size_t)(4410 - i < 37 ? 4410 - i : 37));
    }
    CHECK(g_n == n_once && memcmp(once, g_out, n_once * 2) == 0, "chunked feeding matches one feed");

    int16_t st[4] = { 100, 300, -200, -400 };
    reset();
    gadget_pcm_conv_init(&c, 16000, 2, 16000, collect, NULL);
    gadget_pcm_conv_feed(&c, st, 2);
    CHECK(g_n == 1 && g_out[0] == 200, "stereo is mixed to mono");

    reset();
    g_stop_after = 10;
    gadget_pcm_conv_init(&c, 16000, 1, 16000, collect, NULL);
    CHECK(gadget_pcm_conv_feed(&c, in, 16000) == 7, "the emit's stop value comes back");
}

static void test_wav(void)
{
    static uint8_t wav[200000];
    gadget_wav_t w;

    size_t len = make_wav(wav, 24000, 1, 24000, false);
    reset();
    gadget_wav_init(&w, 16000, collect, NULL);
    CHECK(gadget_wav_feed(&w, wav, len) == GADGET_WAV_OK, "24 kHz mono WAV in one piece");
    CHECK(w.rate == 24000 && w.channels == 1, "header read");
    CHECK(g_n >= 15990 && g_n <= 16000, "a second of audio comes out as 16000 samples");

    /* Byte by byte: header split anywhere, frames split. */
    static int16_t whole[20000];
    size_t n_whole = g_n;
    memcpy(whole, g_out, n_whole * 2);
    reset();
    gadget_wav_init(&w, 16000, collect, NULL);
    bool ok = true;
    for (size_t i = 0; i < len; i += (i % 5) + 1) {
        size_t k = (i % 5) + 1;
        ok &= gadget_wav_feed(&w, wav + i, k <= len - i ? k : len - i) == GADGET_WAV_OK;
    }
    CHECK(ok && g_n == n_whole && memcmp(whole, g_out, n_whole * 2) == 0, "odd-sized chunks give the same samples");

    len = make_wav(wav, 48000, 2, 4800, true);
    reset();
    gadget_wav_init(&w, 16000, collect, NULL);
    CHECK(gadget_wav_feed(&w, wav, len) == GADGET_WAV_OK && w.channels == 2 && w.rate == 48000,
          "48 kHz stereo with a LIST chunk before fmt");
    CHECK(g_n >= 1595 && g_n <= 1600, "100 ms of stereo comes out as 1600 mono samples");

    reset();
    gadget_wav_init(&w, 16000, collect, NULL);
    CHECK(gadget_wav_feed(&w, (const uint8_t *)"ID3\x04\x00 not a wav at all", 22) == GADGET_WAV_NOT_WAV,
          "an MP3 is refused");

    len = make_wav(wav, 16000, 1, 100, false);
    wav[12 + 8 + 14] = 8;   /* bits per sample = 8 */
    gadget_wav_init(&w, 16000, collect, NULL);
    CHECK(gadget_wav_feed(&w, wav, len) == GADGET_WAV_NOT_WAV, "8-bit WAV is refused");

    len = make_wav(wav, 16000, 1, 16000, false);
    reset();
    g_stop_after = 100;
    gadget_wav_init(&w, 16000, collect, NULL);
    CHECK(gadget_wav_feed(&w, wav, len) == GADGET_WAV_STOPPED, "a stop from the emit is reported");

    /* Impo's /tts: the data chunk has a real size and another chunk follows the samples. */
    len = make_wav(wav, 16000, 1, 1600, false);
    size_t data_at = 12 + 24 + 8;
    wav[data_at - 4] = (uint8_t)(3200 & 0xff);
    wav[data_at - 3] = (uint8_t)(3200 >> 8);
    wav[data_at - 2] = wav[data_at - 1] = 0;
    for (size_t i = 0; i < 6070; i++) {
        wav[len + i] = (uint8_t)(0x55 + i * 37);   /* junk, loud as PCM */
    }
    reset();
    gadget_wav_init(&w, 16000, collect, NULL);
    ok = true;
    for (size_t i = 0; i < len + 6070; i += 1000) {
        ok &= gadget_wav_feed(&w, wav + i, len + 6070 - i < 1000 ? len + 6070 - i : 1000) == GADGET_WAV_OK;
    }
    int peak_tail = 0;
    for (size_t i = g_n > 50 ? g_n - 50 : 0; i < g_n; i++) {
        peak_tail = abs(g_out[i]) > peak_tail ? abs(g_out[i]) : peak_tail;
    }
    CHECK(ok && g_n >= 1595 && g_n <= 1600 && peak_tail <= 10000, "bytes after the data chunk are not played");
}

static void test_base64_rms(void)
{
    char out[64];
    size_t n = gadget_base64((const uint8_t *)"Man", 3, out);
    CHECK(n == 4 && memcmp(out, "TWFu", 4) == 0, "base64 of 3 bytes");
    n = gadget_base64((const uint8_t *)"Ma", 2, out);
    CHECK(n == 4 && memcmp(out, "TWE=", 4) == 0, "base64 with one pad");
    n = gadget_base64((const uint8_t *)"M", 1, out);
    CHECK(n == 4 && memcmp(out, "TQ==", 4) == 0, "base64 with two pads");
    n = gadget_base64((const uint8_t *)"\xff\xfe\x00\x01\x02\x03", 6, out);
    CHECK(n == 8 && memcmp(out, "//4AAQID", 8) == 0, "base64 of binary");

    int16_t sq[4] = { 1000, -1000, 1000, -1000 };
    CHECK(gadget_pcm_rms(sq, 4) == 1000, "rms of a square wave");
    int16_t big[2] = { 32767, -32768 };
    CHECK(gadget_pcm_rms(big, 2) >= 32767, "rms at full scale does not overflow");
    CHECK(gadget_pcm_rms(sq, 0) == 0, "rms of nothing");
}

int main(void)
{
    test_resample_counts();
    test_wav();
    test_base64_rms();
    DONE();
}
