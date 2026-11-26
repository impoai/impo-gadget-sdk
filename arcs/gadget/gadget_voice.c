/*
 * Voice turns, as in impo-gadget-sdk (impo_voice.c, impo_chat_session.cpp):
 * the trigger (the board's AUX button) starts listening; the gadget ends the
 * turn itself after a pause (gadget_voice_logic), uploads the speech as a WAV
 * voice note in POST /chat/stream, and speaks the agent's reply that arrives
 * on the long-lived POST /chat/subscribe stream (via /tts, gadget_speech).
 *
 * Tasks: the mic callback (audio driver) fills a stream buffer; the voice
 * task listens, uploads and speaks; the link task runs the stream sinks,
 * which hand replies to the voice task through a queue.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "stream_buffer.h"
#include "task.h"

#include "platform_arcs.h"
#include "impo_link.h"
#include "gadget_pcm.h"
#include "gadget_sound.h"
#include "gadget_speech.h"
#include "gadget_voice.h"
#include "gadget_voice_logic.h"

#define TAG "gadget_voice"
#include "lisa_log.h"

#define FRAME             320                 /* 20 ms at 16 kHz */
#define FRAME_MS          20
#define MIC_BUFFER_MS     1000                /* no network waits while the mic is running */
#define STAGE_BYTES       6144                /* PCM per upload chunk: 8192 base64 chars */
#define UPLOAD_WAIT_MS    120000
#define REPLY_WAIT_MS     120000
#define TURN_QUIET_MS     3000                /* after the last reply is spoken */
#define LINE_MAX          (16 * 1024)

static const char NOTE_HEAD[] =
    "{\"message\":\"\",\"output_modality\":\"text\",\"items\":[{\"type\":\"file\",\"mime_type\":\"audio/wav\","
    "\"filename\":\"voice_note.wav\",\"data_base64\":\"";
static const char NOTE_TAIL[] = "\"}]}";

/* A streaming recorder's header: 16 kHz mono 16-bit, sizes unknown. */
static const uint8_t WAV_HEAD[44] = {
    'R', 'I', 'F', 'F', 0xff, 0xff, 0xff, 0xff, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
    16, 0, 0, 0, 1, 0, 1, 0, 0x80, 0x3e, 0, 0, 0, 0x7d, 0, 0, 2, 0, 16, 0,
    'd', 'a', 't', 'a', 0xff, 0xff, 0xff, 0xff,
};

static TaskHandle_t s_task;
static StreamBufferHandle_t s_mic;
static QueueHandle_t s_replies;              /* char *, from the link task */
static volatile gadget_voice_state_t s_state = GADGET_VOICE_IDLE;
static volatile bool s_touch;
static volatile bool s_continuous, s_session, s_stop, s_wake, s_wakeup_available;
static bool s_followup;
static volatile uint32_t s_mic_dropped;
static volatile uint32_t s_turns, s_replies_spoken;
static char *volatile s_test_text;           /* loopback test: speak this into our own mic */

/* /chat/stream: the ack (link task writes, voice task reads once acked) */
static gadget_chat_ack_t s_ack;
static volatile bool s_acked;
static volatile bool s_note_failed;
static char *s_ack_buf;
static size_t s_ack_len;
static int s_note_status;
static int s_note = -1;

/* /chat/subscribe */
static volatile bool s_sub_open;
static gadget_lines_t s_lines;
static gadget_chat_t s_chat;

/* ---- mic ---- */

static void mic_cb(const int16_t *pcm, size_t n)
{
    size_t want = n * sizeof(int16_t);
    size_t sent = xStreamBufferSend(s_mic, pcm, want, 0);
    if (sent < want) {
        s_mic_dropped += (uint32_t)(want - sent);
    }
}

/* ---- /chat/stream sink ---- */

static void note_begin(void)
{
    s_note_failed = false;
    s_ack_len = 0;
    s_note_status = 0;
    s_acked = false;
}

static void note_status(int status)
{
    s_note_status = status;
}

static void note_data(const unsigned char *d, size_t len)
{
    if (s_ack_buf == NULL) {
        s_ack_buf = malloc(LINE_MAX);
    }
    if (s_ack_buf != NULL && s_ack_len + len < LINE_MAX) {
        memcpy(s_ack_buf + s_ack_len, d, len);
        s_ack_len += len;
    }
}

static void note_end(const char *error)
{
    if (s_ack_buf != NULL) {
        s_ack_buf[s_ack_len] = '\0';
    }
    if (error != NULL || s_note_status != 200 || s_ack_buf == NULL) {
        LISA_LOGW(TAG, "voice note failed: %d %s %.200s", s_note_status, error ? error : "",
                  s_ack_buf ? s_ack_buf : "");
        s_note_failed = true;
        return;
    }
    gadget_chat_ack_t ack;
    if (gadget_chat_parse_ack(s_ack_buf, &ack)) {
        s_ack = ack;
        s_acked = true;
    }
    LISA_LOGI(TAG, "voice note sent: message %s", s_acked ? s_ack.message_id : "?");
}

static const impo_link_stream_sink_t s_note_sink = { note_begin, note_status, note_data, note_end };

/* ---- /chat/subscribe sink ---- */

static void on_line(void *ctx, char *line)
{
    (void)ctx;
    char parent[GADGET_CHAT_ID_MAX];
    char *text = gadget_chat_feed(&s_chat, line, parent);
    if (text == NULL) {
        return;
    }
    bool waiting = s_state == GADGET_VOICE_THINKING || s_state == GADGET_VOICE_SPEAKING;
    if (!waiting || !gadget_chat_reply_is_ours(parent, s_acked ? &s_ack : NULL) ||
        xQueueSend(s_replies, &text, 0) != pdTRUE) {
        LISA_LOGI(TAG, "reply not for this turn: %.60s", text);
        free(text);
    }
}

static void sub_begin(void)
{
    static char *buf;
    if (buf == NULL) {
        buf = malloc(LINE_MAX);
    }
    gadget_lines_init(&s_lines, buf, buf ? LINE_MAX : 0, on_line, NULL);
    gadget_chat_init(&s_chat);
}

static void sub_status(int status)
{
    LISA_LOGI(TAG, "/chat/subscribe status %d", status);
}

static void sub_data(const unsigned char *d, size_t len)
{
    if (s_lines.cap > 0) {
        gadget_lines_feed(&s_lines, d, len);
    }
}

static void sub_end(const char *error)
{
    LISA_LOGW(TAG, "/chat/subscribe ended: %s", error ? error : "by Impo");
    s_sub_open = false;
}

static const impo_link_stream_sink_t s_sub_sink = { sub_begin, sub_status, sub_data, sub_end };

/* ---- one turn ---- */

static void free_replies(void)
{
    char *t;
    while (xQueueReceive(s_replies, &t, 0) == pdTRUE) {
        free(t);
    }
}

static void end_turn(bool stop_sound)
{
    impo_link_stream_cancel(s_note);
    s_note = -1;
    if (stop_sound) {
        gadget_sound_stop();
    }
    free_replies();
    s_state = GADGET_VOICE_IDLE;
}

static void wait_sound_done(uint32_t max_ms)
{
    for (uint32_t t = 0; t < max_ms; t += 20) {
        gadget_sound_state_t st = gadget_sound_state();
        if (st != GADGET_SOUND_PLAYING && st != GADGET_SOUND_FETCHING) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* Capture a complete bounded note before any upload can block this task.
 * Large allocations use PSRAM; the mic queue only bridges audio dispatch jitter. */
static bool listen(const char *test_text)
{
    static uint8_t *note;
    static char *enc;
    const size_t capacity = sizeof(WAV_HEAD) + GADGET_SOUND_RATE * 2u * GADGET_VAD_MAX_MS / 1000u;
    if (note == NULL) {
        note = malloc(capacity);
    }
    if (enc == NULL) {
        enc = malloc(STAGE_BYTES / 3 * 4 + sizeof(NOTE_TAIL) + 4);
    }
    if (note == NULL || enc == NULL || impo_link_state() != IMPO_LINK_ONLINE) {
        gadget_sound_beep_down();
        return false;
    }
    size_t recorded = sizeof(WAV_HEAD);
    memcpy(note, WAV_HEAD, sizeof(WAV_HEAD));
    if (test_text == NULL) {
        gadget_sound_beep();
        wait_sound_done(1000);
        vTaskDelay(pdMS_TO_TICKS(150));
    } else {
        gadget_speech_say(test_text);
        for (int i = 0; i < 1200 && gadget_sound_state() != GADGET_SOUND_PLAYING; i++) {
            if (gadget_sound_state() == GADGET_SOUND_FAILED) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (gadget_sound_state() != GADGET_SOUND_PLAYING) {
            LISA_LOGW(TAG, "self-test speech did not become ready; recording skipped");
            end_turn(true);
            return false;
        }
    }
    xStreamBufferReset(s_mic);
    s_mic_dropped = 0;
    s_touch = false;
    s_state = GADGET_VOICE_LISTENING;
    if (plat_audio_mic_start(mic_cb) != 0) {
        end_turn(true);
        return false;
    }
    LISA_LOGI(TAG, "listening");
    s_turns++;

    gadget_vad_t vad;
    gadget_vad_init(&vad);
    gadget_vad_result_t vr = GADGET_VAD_LISTENING;
    int16_t frame[FRAME];
    size_t have = 0;
    uint32_t wall0 = (uint32_t)plat_uptime_ms();
    const char *why = NULL;
    while (vr == GADGET_VAD_LISTENING) {
        have += xStreamBufferReceive(s_mic, (uint8_t *)frame + have, sizeof(frame) - have, pdMS_TO_TICKS(200));
        if (s_touch || s_stop) {
            why = "touched";
            break;
        }
        if (have < sizeof(frame)) {
            if ((uint32_t)plat_uptime_ms() - wall0 >= GADGET_VAD_MAX_MS + 2000u) {
                why = "mic timeout";
                break;
            }
            continue;
        }
        have = 0;
        vr = gadget_vad_frame(&vad, gadget_pcm_rms(frame, FRAME), FRAME_MS);
        if (recorded + sizeof(frame) > capacity) {
            why = "record limit";
            break;
        }
        memcpy(note + recorded, frame, sizeof(frame));
        recorded += sizeof(frame);
    }
    plat_audio_mic_stop();
    s_touch = false;
    if (why == NULL) {
        why = vr == GADGET_VAD_PAUSE ? "pause" : vr == GADGET_VAD_NO_SPEECH ? "no speech" : "max length";
    }
    LISA_LOGI(TAG, "listened %u ms in %u ms (floor %u, threshold %u), ended by %s; mic dropped %u bytes",
              (unsigned)vad.t_ms, (unsigned)((uint32_t)plat_uptime_ms() - wall0), (unsigned)vad.floor_min,
              (unsigned)vad.threshold, why, (unsigned)s_mic_dropped);
    if (s_stop || !vad.heard || s_mic_dropped != 0 || strcmp(why, "mic timeout") == 0) {
        end_turn(test_text != NULL);
        gadget_sound_beep_down();
        return false;
    }
    if (test_text == NULL) {
        gadget_sound_beep_down();
    } else {
        wait_sound_done(15000);
    }
    /* Exact RIFF/data lengths: the complete PCM note is now in memory. */
    uint32_t pcm_size = (uint32_t)(recorded - sizeof(WAV_HEAD));
    for (unsigned i = 0; i < 4; i++) {
        note[4 + i] = (uint8_t)((pcm_size + 36u) >> (8u * i));
        note[40 + i] = (uint8_t)(pcm_size >> (8u * i));
    }
    s_state = GADGET_VOICE_THINKING;
    s_acked = false;
    s_note = impo_link_stream_open("/chat/stream", "application/json", NULL, NOTE_HEAD, strlen(NOTE_HEAD), true,
                                   &s_note_sink);
    if (s_note < 0) {
        end_turn(false);
        return false;
    }
    uint64_t upload0 = plat_uptime_ms();
    for (size_t off = 0; off < recorded;) {
        if (s_touch || s_note_failed || plat_uptime_ms() - upload0 >= UPLOAD_WAIT_MS) {
            LISA_LOGW(TAG, "note upload cancelled: %u/%u bytes queued", (unsigned)off, (unsigned)recorded);
            end_turn(true);
            return false;
        }
        size_t n = recorded - off < STAGE_BYTES ? recorded - off : STAGE_BYTES;
        size_t e = gadget_base64(note + off, n, enc);
        bool last = off + n == recorded;
        if (last) {
            memcpy(enc + e, NOTE_TAIL, sizeof(NOTE_TAIL) - 1);
            e += sizeof(NOTE_TAIL) - 1;
        }
        if (impo_link_stream_send(s_note, enc, e, last) != 0) {
            LISA_LOGW(TAG, "note upload failed: %u/%u bytes queued", (unsigned)off, (unsigned)recorded);
            end_turn(true);
            return false;
        }
        off += n;
    }
    LISA_LOGI(TAG, "note queued: %u PCM bytes in %u ms", (unsigned)pcm_size,
              (unsigned)(plat_uptime_ms() - upload0));
    s_note = -1;   /* the link finishes it; note_end reports server acceptance */
    return true;
}

/* Speaks replies in order until a quiet spell; a touch ends it early. */
static bool await_replies(void)
{
    s_state = GADGET_VOICE_THINKING;
    uint32_t waited = 0, quiet = 0;
    bool spoke = false, completed = false;
    char *current = NULL;      /* the reply being spoken, kept for one retry */
    bool retried = false;
    for (;;) {
        if (s_touch) {
            free(current);
            return false;   /* caller handles cancellation */
        }
        gadget_sound_state_t st = gadget_sound_state();
        bool busy = st == GADGET_SOUND_FETCHING || st == GADGET_SOUND_PLAYING;
        /* The link dropped while fetching the speech (a router reset): say it again once it is back. */
        if (current != NULL && st == GADGET_SOUND_FAILED && !retried && impo_link_state() == IMPO_LINK_ONLINE) {
            LISA_LOGW(TAG, "speech failed (%s); saying the reply again", gadget_sound_error());
            retried = true;
            gadget_speech_say(current);
            busy = true;
        }
        char *text = NULL;
        if (!busy && (st != GADGET_SOUND_FAILED || retried || current == NULL) &&
            xQueueReceive(s_replies, &text, 0) == pdTRUE) {
            LISA_LOGI(TAG, "reply: %.120s", text);
            if (gadget_speech_say(text) != 0) {
                free(text); free(current);
                s_state = GADGET_VOICE_IDLE;
                return false;
            }
            free(current);
            current = text;
            retried = false;
            s_state = GADGET_VOICE_SPEAKING;
            s_replies_spoken++;
            spoke = true;
            busy = true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
        quiet = busy ? 0 : quiet + 100;
        if (spoke && quiet >= TURN_QUIET_MS && uxQueueMessagesWaiting(s_replies) == 0) {
            completed = st != GADGET_SOUND_FAILED && impo_link_state() == IMPO_LINK_ONLINE;
            break;
        }
        if (!spoke && s_note_failed) {
            LISA_LOGW(TAG, "Impo did not take the voice note");
            gadget_sound_beep_down();
            break;
        }
        if (!spoke && waited >= REPLY_WAIT_MS) {
            LISA_LOGW(TAG, "no reply from Impo");
            gadget_sound_beep_down();
            break;
        }
        if (spoke && waited >= REPLY_WAIT_MS * 2) {
            break;   /* a failed retry must not hold the turn forever */
        }
    }
    free(current);
    s_state = GADGET_VOICE_IDLE;
    return completed;
}

static void run_requested_turn(void)
{
    char *test = s_test_text;
    s_test_text = NULL;
    s_touch = s_wake = false;
    bool followup = s_followup;
    s_followup = false;
    s_session = test == NULL && s_continuous;
    end_turn(true);
    if (followup) vTaskDelay(pdMS_TO_TICKS(300)); /* speaker tail */
    bool complete = !s_stop && listen(test) && await_replies();
    s_followup = test == NULL && s_session && complete && !s_stop;
    if (!s_followup) {
        end_turn(true);
        s_session = false;
        /* A click while in a continuous session exits, never starts another turn. */
        if (s_stop) s_touch = false;
        s_stop = false;
    }
    free(test);
}

static void voice_task(void *arg)
{
    (void)arg;
    uint32_t sub_retry_ms = 0;
    for (;;) {
        /* Keep the reply subscription open while online. */
        if (!s_sub_open && impo_link_state() == IMPO_LINK_ONLINE && sub_retry_ms == 0) {
            s_sub_open = impo_link_stream_open("/chat/subscribe", "application/json", "application/x-ndjson", "{}",
                                               2, false, &s_sub_sink) >= 0;
            sub_retry_ms = 5000;
        }
        if (s_stop) {
            end_turn(true);
            s_session = s_followup = s_wake = s_touch = s_stop = false;
        }
        if (s_touch || s_wake || s_followup || s_test_text != NULL) {
            run_requested_turn();
            continue;
        }
        /* Plain sleep: the mic stream buffer uses this task's notification. */
        vTaskDelay(pdMS_TO_TICKS(50));
        sub_retry_ms = sub_retry_ms > 50 ? sub_retry_ms - 50 : 0;
    }
}

/* ---- public ---- */

void gadget_voice_trigger(void)
{
    if (s_session) s_stop = true;
    s_touch = true;
}
void gadget_voice_set_continuous(bool enabled) { s_continuous = enabled; }
bool gadget_voice_continuous_active(void) { return s_session; }
void gadget_voice_set_wakeup_available(bool available) { s_wakeup_available = available; }
bool gadget_voice_wakeup_available(void) { return s_wakeup_available; }
void gadget_voice_wake(void)
{
    if (s_state == GADGET_VOICE_IDLE && !s_session && !s_test_text) s_wake = true;
}

int gadget_voice_test(const char *text)
{
    if (s_state != GADGET_VOICE_IDLE || s_session || s_test_text != NULL) {
        return -1;
    }
    char *copy = strdup(text);
    s_test_text = copy;
    return copy != NULL ? 0 : -1;
}

gadget_voice_state_t gadget_voice_state(void)
{
    return s_state;
}

void gadget_voice_counts(uint32_t *turns, uint32_t *replies)
{
    *turns = s_turns;
    *replies = s_replies_spoken;
}

int gadget_voice_start(void)
{
    s_mic = xStreamBufferCreate(GADGET_SOUND_RATE * 2 * MIC_BUFFER_MS / 1000, sizeof(int16_t) * FRAME);
    s_replies = xQueueCreate(4, sizeof(char *));
    if (s_mic == NULL || s_replies == NULL) {
        LISA_LOGE(TAG, "no memory for the voice buffers");
        return -1;
    }
    if (xTaskCreate(voice_task, "gadget_voice", 8192 / sizeof(StackType_t), NULL, 3, &s_task) != pdPASS) {
        return -1;
    }
    LISA_LOGI(TAG, "voice ready: touch to talk");
    return 0;
}
