/* Run the SDK's actual listen() with audio frames and a deliberately slow link. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "gadget_pcm.h"
static size_t capture_base64(const uint8_t *, size_t, char *);
#define gadget_base64 capture_base64
#include "../../gadget/gadget_voice.c"
#undef gadget_base64

static uint64_t clock_ms;
static unsigned frame_n, send_n, opened, cancelled, final_n, per_send_ms;
static unsigned fail_send;
static bool mic_running, speech = true, missing_frames;
static bool answer, reply_given, say_fails;
static uint64_t playing_until;
static unsigned interrupt_frame;
static uint8_t raw_note[500000];
static size_t raw_n;

static int16_t sample_at(unsigned i)
{ return speech && i >= 25 && i < 60 ? (int16_t)(2000 + i) : 100; }

uint64_t plat_uptime_ms(void) { return clock_ms; }
void vTaskDelay(unsigned ms) { clock_ms += ms; }
void test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
int xTaskCreate(void (*f)(void *), const char *n, unsigned d, void *a, unsigned p, TaskHandle_t *h)
{ (void)f; (void)n; (void)d; (void)a; (void)p; (void)h; return pdPASS; }
QueueHandle_t xQueueCreate(unsigned n, unsigned size) { (void)n; (void)size; return (void *)1; }
int xQueueSend(QueueHandle_t q, const void *p, unsigned ms)
{ (void)q; (void)p; (void)ms; return pdTRUE; }
int xQueueReceive(QueueHandle_t q, void *p, unsigned ms)
{ (void)q; (void)ms;
  if (answer && final_n && !reply_given) { *(char **)p = strdup("five"); reply_given=true; return pdTRUE; }
  return pdFALSE; }
unsigned uxQueueMessagesWaiting(QueueHandle_t q) { (void)q; return 0; }
StreamBufferHandle_t xStreamBufferCreate(size_t n, size_t trigger)
{ (void)n; (void)trigger; return (void *)1; }
int xStreamBufferReset(StreamBufferHandle_t q) { (void)q; return pdTRUE; }
size_t xStreamBufferSend(StreamBufferHandle_t q, const void *p, size_t n, unsigned ms)
{ (void)q; (void)p; (void)ms; return n; }
size_t xStreamBufferReceive(StreamBufferHandle_t q, void *p, size_t n, unsigned ms)
{
    (void)q; assert(mic_running && opened == 0 && send_n == 0);
    if (missing_frames) { clock_ms += ms; return 0; }
    assert(n == FRAME * sizeof(int16_t));
    int16_t *samples = p;
    for (unsigned i = 0; i < FRAME; i++) samples[i] = sample_at(frame_n);
    frame_n++; clock_ms += FRAME_MS;
    if (interrupt_frame && frame_n == interrupt_frame) gadget_voice_trigger();
    return n;
}
int plat_audio_mic_start(plat_audio_mic_cb_t cb)
{ (void)cb; assert(!mic_running); mic_running = true; return 0; }
void plat_audio_mic_stop(void) { assert(mic_running); mic_running = false; }
int gadget_sound_beep(void) { return 0; }
int gadget_sound_beep_down(void) { return 0; }
void gadget_sound_stop(void) {}
gadget_sound_state_t gadget_sound_state(void) { return clock_ms < playing_until ? GADGET_SOUND_PLAYING : GADGET_SOUND_IDLE; }
const char *gadget_sound_error(void) { return ""; }
int gadget_speech_say(const char *text) { (void)text; if (say_fails) return -1; playing_until=clock_ms+200; return 0; }
impo_link_state_t impo_link_state(void) { return IMPO_LINK_ONLINE; }
int impo_link_stream_open(const char *path, const char *type, const char *accept,
                          const void *body, size_t n, bool more, const impo_link_stream_sink_t *sink)
{
    (void)accept;
    assert(!mic_running && frame_n >= 120);
    assert(strcmp(path, "/chat/stream") == 0 && strcmp(type, "application/json") == 0);
    assert(more && n == strlen(NOTE_HEAD) && memcmp(body, NOTE_HEAD, n) == 0);
    opened++; sink->on_begin(); return 17;
}
int impo_link_stream_send(int h, const void *data, size_t n, bool end)
{
    assert(h == 17 && !mic_running && opened == 1);
    send_n++; clock_ms += per_send_ms;
    if (send_n == fail_send) return -1;
    if (end) {
        assert(n >= sizeof(NOTE_TAIL) - 1);
        assert(memcmp((const char *)data + n - sizeof(NOTE_TAIL) + 1, NOTE_TAIL, sizeof(NOTE_TAIL) - 1) == 0);
        final_n++;
    }
    return 0;
}
void impo_link_stream_cancel(int h) { if (h >= 0) cancelled++; }
static size_t capture_base64(const uint8_t *in, size_t n, char *out)
{
    assert(!mic_running && raw_n + n <= sizeof(raw_note));
    memcpy(raw_note + raw_n, in, n); raw_n += n;
    return gadget_base64(in, n, out);
}
static void reset(void)
{
    clock_ms = 0; frame_n = send_n = opened = cancelled = final_n = 0;
    raw_n = 0; per_send_ms = 6000; fail_send = 0;
    speech = true; missing_frames = false; s_touch = false; s_note = -1;
    s_note_failed = false; s_state = GADGET_VOICE_IDLE;
    s_continuous=s_session=s_stop=s_wake=s_followup=false;
    answer=reply_given=say_fails=false; playing_until=0; interrupt_frame=0;
}
static uint32_t u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
int main(void)
{
    reset();
    assert(listen(NULL));
    assert(frame_n == 120 && opened == 1 && final_n == 1 && cancelled == 0);
    assert(raw_n == 44 + frame_n * FRAME * 2 && !mic_running);
    assert(u32(raw_note + 4) == raw_n - 8 && u32(raw_note + 40) == raw_n - 44);
    for (unsigned i = 0; i < frame_n; i++) {
        const int16_t *pcm = (const int16_t *)(raw_note + 44 + i * FRAME * 2);
        for (unsigned j = 0; j < FRAME; j++) assert(pcm[j] == sample_at(i));
    }
    reset(); speech = false;
    assert(!listen(NULL) && opened == 0 && !mic_running);
    reset(); fail_send = 2;
    assert(!listen(NULL) && cancelled == 1 && final_n == 0 && !mic_running);
    reset(); per_send_ms = 30000;
    assert(!listen(NULL) && cancelled == 1 && send_n == 4 && final_n == 0);
    reset(); missing_frames = true;
    assert(!listen(NULL) && opened == 0 && !mic_running);
    /* Execute the actual task turn handler: one click, two completed replies,
     * then five seconds of silence releases the continuous session. */
    reset(); per_send_ms=0; answer=true;
    gadget_voice_set_continuous(true); gadget_voice_trigger();
    run_requested_turn();
    assert(s_session && s_followup && !s_touch && reply_given);
    frame_n=send_n=opened=final_n=0; raw_n=0; reply_given=false;
    run_requested_turn();
    assert(s_session && s_followup && reply_given);
    frame_n=send_n=opened=final_n=0; raw_n=0; speech=false; reply_given=false;
    run_requested_turn();
    assert(!s_session && !s_followup && !mic_running && opened==0 && frame_n==250);
    /* A click while speaking/recording exits without uploading partial speech. */
    reset(); per_send_ms=0; answer=true; interrupt_frame=40;
    gadget_voice_set_continuous(true); gadget_voice_wake(); run_requested_turn();
    assert(!s_session && !s_followup && !s_touch && !mic_running && opened==0);
    reset(); per_send_ms=0; answer=true; say_fails=true;
    gadget_voice_set_continuous(true); gadget_voice_trigger(); run_requested_turn();
    assert(!s_session && !s_followup);
    reset(); per_send_ms=0; answer=true; gadget_voice_trigger(); run_requested_turn();
    assert(!s_session && !s_followup && reply_given); /* other boards remain one shot */
    puts("Continuous voice: two turns, silent exit, click cancellation, TTS failure and opt-in passed");
    puts("Voice capture: full PCM before upload, exact WAV sizes, slow upload, silence, failure and deadline passed");
}
