/* Host tests for gadget/gadget_voice_logic.c: end of speech, lines, chat replies. */
#include <stdlib.h>
#include <string.h>

#include "host_check.h"
#include "gadget_voice_logic.h"

/* Runs frames of the given rms until the detector decides; returns the result and the time. */
static gadget_vad_result_t run(gadget_vad_t *v, const uint32_t *rms, int n, uint32_t *t)
{
    gadget_vad_result_t r = GADGET_VAD_LISTENING;
    for (int i = 0; i < n && r == GADGET_VAD_LISTENING; i++) {
        r = gadget_vad_frame(v, rms[i], 20);
    }
    *t = v->t_ms;
    return r;
}

static void test_vad(void)
{
    static uint32_t f[2000];
    gadget_vad_t v;
    uint32_t t;

    /* Quiet room: nothing heard, gives up after 5 s. */
    for (int i = 0; i < 2000; i++) f[i] = 50;
    gadget_vad_init(&v);
    CHECK(run(&v, f, 2000, &t) == GADGET_VAD_NO_SPEECH && t == GADGET_VAD_NO_SPEECH_MS, "silence ends at 5 s");
    CHECK(v.threshold == GADGET_VAD_MIN_RMS, "a quiet floor keeps the minimum threshold");

    /* Speech from 1 s to 3 s, then quiet: ends 1.2 s after it stops. */
    for (int i = 0; i < 2000; i++) f[i] = (i >= 50 && i < 150) ? 4000 : 50;
    gadget_vad_init(&v);
    CHECK(run(&v, f, 2000, &t) == GADGET_VAD_PAUSE && t == 3000 + GADGET_VAD_PAUSE_MS, "a pause after speech ends it");

    /* Short gaps inside speech do not end it. */
    for (int i = 0; i < 2000; i++) f[i] = (i >= 50 && i < 300 && (i % 40) < 30) ? 4000 : 50;
    gadget_vad_init(&v);
    CHECK(run(&v, f, 2000, &t) == GADGET_VAD_PAUSE && t > 6000, "200 ms gaps between words keep listening");

    /* Never stops talking: capped at 15 s. */
    for (int i = 0; i < 2000; i++) f[i] = 4000;
    gadget_vad_init(&v);
    CHECK(run(&v, f, 2000, &t) == GADGET_VAD_MAX_LENGTH && t == GADGET_VAD_MAX_MS, "15 s at most");

    /* The start beep's tail in the first frames does not raise the floor. */
    for (int i = 0; i < 2000; i++) f[i] = i < 10 ? 6000 : (i >= 60 && i < 120 ? 2000 : 300);
    gadget_vad_init(&v);
    gadget_vad_result_t r = run(&v, f, 2000, &t);
    CHECK(v.threshold == 900, "floor is the quietest calibration frame, x3");
    CHECK(r == GADGET_VAD_PAUSE, "speech above a modest floor is heard");

    /* Clicks (a servo, the amplifier switching on) are not speech. */
    for (int i = 0; i < 2000; i++) f[i] = (i == 60 || i == 61 || i == 120) ? 9000 : 14;
    gadget_vad_init(&v);
    CHECK(run(&v, f, 2000, &t) == GADGET_VAD_NO_SPEECH, "short clicks in a quiet room do not start speech");
    for (int i = 0; i < 2000; i++) f[i] = (i >= 60 && i < 75) ? 9000 : 14;
    gadget_vad_init(&v);
    CHECK(run(&v, f, 2000, &t) == GADGET_VAD_PAUSE, "300 ms of sound does");

    /* A loud floor (servo hum) is capped so speech can still count. */
    for (int i = 0; i < 2000; i++) f[i] = (i >= 50 && i < 100) ? 8000 : 2500;
    gadget_vad_init(&v);
    run(&v, f, 2000, &t);
    CHECK(v.threshold == GADGET_VAD_MAX_RMS, "threshold capped at 3000");
}

static char g_lines[8][64];
static int g_nlines;

static void on_line(void *ctx, char *line)
{
    (void)ctx;
    if (g_nlines < 8) {
        strncpy(g_lines[g_nlines], line, 63);
    }
    g_nlines++;
}

static void test_lines(void)
{
    char buf[16];
    gadget_lines_t l;
    gadget_lines_init(&l, buf, sizeof(buf), on_line, NULL);
    g_nlines = 0;
    const char *s = "ab\ncd\r\n\nthis line is far too long\nok\npart";
    for (const char *p = s; *p; p++) {
        gadget_lines_feed(&l, (const uint8_t *)p, 1);
    }
    CHECK(g_nlines == 3, "three lines: empty and overlong ones skipped, last one incomplete");
    CHECK(strcmp(g_lines[0], "ab") == 0 && strcmp(g_lines[1], "cd") == 0 && strcmp(g_lines[2], "ok") == 0,
          "contents, CR stripped");
    gadget_lines_feed(&l, (const uint8_t *)"ial\n", 4);
    CHECK(g_nlines == 4 && strcmp(g_lines[3], "partial") == 0, "a line split across chunks");
}

static void test_chat(void)
{
    gadget_chat_ack_t ack;
    CHECK(gadget_chat_parse_ack("{\"result\":{\"message_id\":\"m1\",\"reply_to_message_id\":\"r1\"}}", &ack) &&
              strcmp(ack.message_id, "m1") == 0 && strcmp(ack.reply_to_message_id, "r1") == 0,
          "ack ids under result");
    CHECK(gadget_chat_parse_ack("{\"message_id\":\"m2\"}", &ack) && strcmp(ack.message_id, "m2") == 0,
          "ack ids at the root");
    CHECK(!gadget_chat_parse_ack("{\"error\":\"x\"}", &ack), "an error is not an ack");
    CHECK(!gadget_chat_parse_ack("not json", &ack), "garbage is not an ack");

    char parent[GADGET_CHAT_ID_MAX];
    gadget_chat_t c;
    gadget_chat_init(&c);
    /* As recorded from gadgets.impo.ai: start (with parent), appends, done (with display_text). */
    CHECK(gadget_chat_feed(&c, "{\"type\":\"event\",\"seq\":66,\"event\":\"delta.message_start\",\"payload\":"
                               "{\"message_id\":\"a1\",\"reply_to_message_id\":\"m1\"}}", parent) == NULL,
          "message_start alone gives nothing");
    CHECK(gadget_chat_feed(&c, "{\"type\":\"event\",\"seq\":67,\"event\":\"delta.text_append\",\"payload\":"
                               "{\"message_id\":\"a1\",\"text\":\"Two plus three\"}}", parent) == NULL,
          "text_append gives nothing");
    char *t = gadget_chat_feed(&c, "{\"type\":\"event\",\"seq\":68,\"event\":\"delta.message_done\",\"payload\":"
                                   "{\"message_id\":\"a1\",\"display_text\":\"Two plus three is five.\"}}", parent);
    CHECK(t && strcmp(t, "Two plus three is five.") == 0, "message_done hands out the text");
    CHECK(strcmp(parent, "m1") == 0, "with the parent remembered from message_start");
    free(t);
    CHECK(gadget_chat_feed(&c, "{\"type\":\"event\",\"seq\":69,\"event\":\"message.assistant\",\"payload\":"
                               "{\"message_id\":\"a1\",\"display_text\":\"Two plus three is five.\"}}", parent) == NULL,
          "the persisted message.assistant is not spoken twice");

    t = gadget_chat_feed(&c, "{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"id\":\"a2\","
                             "\"content\":\"你好\",\"parent_message_id\":\"r1\"}}", parent);
    CHECK(t && strcmp(t, "你好") == 0 && strcmp(parent, "r1") == 0,
          "message.assistant alone works; content and parent_message_id fallbacks");
    free(t);
    CHECK(gadget_chat_feed(&c, "{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":"
                               "\"a3\",\"display_text\":\"x\",\"display_text_ready\":false}}", parent) == NULL,
          "not ready yet is skipped");
    t = gadget_chat_feed(&c, "{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":"
                             "\"a3\",\"display_text\":\"now ready\"}}", parent);
    CHECK(t && strcmp(t, "now ready") == 0, "and handed out once it is ready");
    free(t);
    CHECK(gadget_chat_feed(&c, "{\"type\":\"subscribed\"}", parent) == NULL, "the subscription ack is skipped");
    CHECK(gadget_chat_feed(&c, "garbage", parent) == NULL, "garbage is skipped");
    for (int i = 0; i < 20; i++) {
        char line[200];
        snprintf(line, sizeof(line), "{\"type\":\"event\",\"event\":\"delta.message_done\",\"payload\":"
                 "{\"message_id\":\"z%d\",\"display_text\":\"t\"}}", i);
        free(gadget_chat_feed(&c, line, parent));
    }
    t = gadget_chat_feed(&c, "{\"type\":\"event\",\"event\":\"delta.message_done\",\"payload\":"
                             "{\"message_id\":\"z19\",\"display_text\":\"t\"}}", parent);
    CHECK(t == NULL, "a recent message is still remembered after many others");

    gadget_chat_parse_ack("{\"message_id\":\"m1\",\"reply_to_message_id\":\"r1\"}", &ack);
    CHECK(gadget_chat_reply_is_ours("m1", &ack) && gadget_chat_reply_is_ours("r1", &ack), "replies to our note match");
    CHECK(!gadget_chat_reply_is_ours("other", &ack), "a reply to something else does not");
    CHECK(gadget_chat_reply_is_ours("other", NULL) && gadget_chat_reply_is_ours("", &ack),
          "before the ack, or without a parent, any reply counts");
}

int main(void)
{
    test_vad();
    test_lines();
    test_chat();
    DONE();
}
