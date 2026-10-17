/*
 * The decisions of a voice turn, with no RTOS or hardware dependency
 * (host-tested): when the speaker has finished talking, and which lines of
 * the /chat/subscribe stream are this turn's replies.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- end of speech ---- */

#define GADGET_VAD_CALIBRATE_MS  500     /* noise floor: the quietest frame in this span */
#define GADGET_VAD_PAUSE_MS      1200    /* this much quiet after speech ends the turn */
#define GADGET_VAD_NO_SPEECH_MS  5000
#define GADGET_VAD_MAX_MS        15000
#define GADGET_VAD_MIN_RMS       500     /* speech threshold bounds */
#define GADGET_VAD_MAX_RMS       3000
#define GADGET_VAD_MIN_SPEECH_MS 300     /* this much above the threshold before it counts as speech: not a click */

typedef enum {
    GADGET_VAD_LISTENING,
    GADGET_VAD_PAUSE,        /* spoke, then went quiet */
    GADGET_VAD_NO_SPEECH,
    GADGET_VAD_MAX_LENGTH,
} gadget_vad_result_t;

typedef struct {
    uint32_t t_ms;
    uint32_t floor_min;
    uint32_t threshold;
    uint32_t quiet_ms;
    uint32_t loud_ms;        /* above the threshold so far */
    bool heard;
} gadget_vad_t;

void gadget_vad_init(gadget_vad_t *v);
/* One frame's rms. The first GADGET_VAD_CALIBRATE_MS set the threshold:
 * 3 x the quietest frame, kept within the bounds. */
gadget_vad_result_t gadget_vad_frame(gadget_vad_t *v, uint32_t rms, uint32_t frame_ms);

/* ---- newline-delimited stream ---- */

typedef struct {
    char *buf;
    size_t len, cap;
    bool overflow;           /* the current line is too long: skipped through its newline */
    void (*on_line)(void *ctx, char *line);
    void *ctx;
} gadget_lines_t;

/* buf of cap bytes is the caller's. */
void gadget_lines_init(gadget_lines_t *l, char *buf, size_t cap, void (*on_line)(void *ctx, char *line), void *ctx);
void gadget_lines_feed(gadget_lines_t *l, const uint8_t *data, size_t len);

/* ---- chat replies ---- */

#define GADGET_CHAT_ID_MAX 64

/* The /chat/stream reply: the ids a reply to this note will point at. */
typedef struct {
    char message_id[GADGET_CHAT_ID_MAX];
    char reply_to_message_id[GADGET_CHAT_ID_MAX];
} gadget_chat_ack_t;

/* Reads {"result": {...ids}} or the ids at the root. False when the JSON has neither. */
bool gadget_chat_parse_ack(const char *json, gadget_chat_ack_t *ack);

/* Follows the assistant's messages on /chat/subscribe and hands out each finished one once.
 * A message streams as delta.message_start (with its parent), delta.text_append...,
 * delta.message_done (with display_text); message.assistant later repeats it in full. */
#define GADGET_CHAT_TRACK 8

typedef struct {
    struct {
        char id[GADGET_CHAT_ID_MAX];
        char parent[GADGET_CHAT_ID_MAX];
        bool done;              /* handed out already */
    } msg[GADGET_CHAT_TRACK];
    unsigned next;              /* ring slot to reuse */
} gadget_chat_t;

void gadget_chat_init(gadget_chat_t *c);

/* One subscription line. When it finishes an assistant message not handed out before, returns its
 * text (malloc'd, the caller frees) and fills parent ("" when unknown); else NULL. */
char *gadget_chat_feed(gadget_chat_t *c, const char *line, char parent[GADGET_CHAT_ID_MAX]);

/* Whether a reply with this parent belongs to the turn. Before the ack (ack NULL) or without a
 * parent, any reply does. */
bool gadget_chat_reply_is_ours(const char *parent, const gadget_chat_ack_t *ack);

#ifdef __cplusplus
}
#endif
