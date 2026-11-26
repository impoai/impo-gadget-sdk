/* SPDX-License-Identifier: MIT
 * Execute the actual TTS stream callbacks with a real WAV parser. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../gadget/gadget_speech.c"

static unsigned ended, samples;
static char error_text[128];
uint32_t gadget_sound_begin_buffered(const char *what)
{ assert(!strcmp(what, "say")); ended = samples = 0; error_text[0] = 0; return 1; }
int gadget_sound_write(uint32_t gen, const int16_t *pcm, size_t count, uint32_t timeout)
{ assert(gen == 1 && pcm && timeout == 5000); samples += count; return 0; }
void gadget_sound_end(uint32_t gen, const char *error)
{ assert(gen == 1); ended++; snprintf(error_text, sizeof(error_text), "%s", error ? error : ""); }
void test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
void impo_link_stream_cancel(int handle) { (void)handle; }
int impo_link_stream_open(const char *path, const char *content, const char *accept,
                         const void *body, size_t len, bool more, const impo_link_stream_sink_t *sink)
{
    assert(!strcmp(path, "/tts") && !strcmp(content, "application/json"));
    assert(!strcmp(accept, "audio/wav") && body && len && !more && sink == &s_sink);
    return 1;
}
static void le32(unsigned char *p, unsigned n)
{ for (unsigned i=0; i<4; i++) { p[i] = n & 255; n >>= 8; } }
static unsigned char wav[52];
static void begin(unsigned data_size)
{
    memset(wav, 0, sizeof(wav));
    memcpy(wav, "RIFF", 4); le32(wav+4, 36+data_size);
    memcpy(wav+8, "WAVEfmt ", 8); le32(wav+16, 16);
    wav[20]=1; wav[22]=1; le32(wav+24,16000); le32(wav+28,32000);
    wav[32]=2; wav[34]=16; memcpy(wav+36,"data",4); le32(wav+40,data_size);
    on_begin(); on_status(200);
}
int main(void)
{
    assert(gadget_speech_say("hello") == 0);
    begin(8); on_data(wav,48); assert(!ended && samples>0);
    on_data(wav+48,4); assert(ended==1 && !*error_text);
    on_data((const unsigned char *)"trailing metadata",17); on_end("late disconnect");
    assert(ended==1 && !*error_text);
    begin(8); on_data(wav,48); on_end(NULL);
    assert(ended==1 && strstr(error_text,"complete"));
    begin(7); on_data(wav,sizeof(wav)); on_end(NULL);
    assert(ended==1 && strstr(error_text,"frame"));
    begin(8); on_data(wav,48); on_end("connection dropped");
    assert(ended==1 && !strcmp(error_text,"connection dropped"));
    begin(8); on_status(401); on_data(wav,sizeof(wav)); on_end(NULL);
    assert(ended==1 && *error_text);
    begin(UINT32_MAX); on_data(wav,47); on_end(NULL);
    assert(ended==1 && *error_text); /* Incomplete PCM frame in an unknown-length WAV. */
    begin(UINT32_MAX); on_data(wav,sizeof(wav)); on_end(NULL);
    assert(ended==1 && !*error_text);
    puts("TTS: complete PCM, trailing metadata, truncation, partial frame, HTTP and network errors passed");
    return 0;
}
