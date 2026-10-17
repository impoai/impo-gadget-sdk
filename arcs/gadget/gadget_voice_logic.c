/* Voice turn decisions; see gadget_voice_logic.h. Host-tested (tests/host/test_gadget_voice_logic.c). */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "gadget_voice_logic.h"

/* ---- end of speech ---- */

void gadget_vad_init(gadget_vad_t *v)
{
    memset(v, 0, sizeof(*v));
    v->floor_min = UINT32_MAX;
    v->threshold = GADGET_VAD_MIN_RMS;
}

gadget_vad_result_t gadget_vad_frame(gadget_vad_t *v, uint32_t rms, uint32_t frame_ms)
{
    v->t_ms += frame_ms;
    if (v->t_ms <= GADGET_VAD_CALIBRATE_MS) {
        /* The minimum ignores the start beep's tail and words said straight away. */
        if (rms < v->floor_min) {
            v->floor_min = rms;
        }
        if (v->t_ms + frame_ms > GADGET_VAD_CALIBRATE_MS) {
            uint32_t th = v->floor_min * 3;
            th = th < GADGET_VAD_MIN_RMS ? GADGET_VAD_MIN_RMS : th;
            v->threshold = th > GADGET_VAD_MAX_RMS ? GADGET_VAD_MAX_RMS : th;
        }
    } else if (rms > v->threshold) {
        v->loud_ms += frame_ms;
        if (v->loud_ms >= GADGET_VAD_MIN_SPEECH_MS) {
            v->heard = true;
        }
        v->quiet_ms = 0;
    } else {
        v->quiet_ms += frame_ms;
    }
    if (v->heard && v->quiet_ms >= GADGET_VAD_PAUSE_MS) {
        return GADGET_VAD_PAUSE;
    }
    if (!v->heard && v->t_ms >= GADGET_VAD_NO_SPEECH_MS) {
        return GADGET_VAD_NO_SPEECH;
    }
    if (v->t_ms >= GADGET_VAD_MAX_MS) {
        return GADGET_VAD_MAX_LENGTH;
    }
    return GADGET_VAD_LISTENING;
}

/* ---- lines ---- */

void gadget_lines_init(gadget_lines_t *l, char *buf, size_t cap, void (*on_line)(void *ctx, char *line), void *ctx)
{
    memset(l, 0, sizeof(*l));
    l->buf = buf;
    l->cap = cap;
    l->on_line = on_line;
    l->ctx = ctx;
}

void gadget_lines_feed(gadget_lines_t *l, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n') {
            if (!l->overflow && l->len > 0) {
                if (l->buf[l->len - 1] == '\r') {
                    l->len--;
                }
                l->buf[l->len] = '\0';
                l->on_line(l->ctx, l->buf);
            }
            l->len = 0;
            l->overflow = false;
        } else if (l->overflow) {
            continue;
        } else if (l->len + 1 < l->cap) {
            l->buf[l->len++] = c;
        } else {
            l->overflow = true;
        }
    }
}

/* ---- chat ---- */

static void copy_id(const cJSON *o, const char *key, char *out)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    if (cJSON_IsString(v) && strlen(v->valuestring) < GADGET_CHAT_ID_MAX) {
        strcpy(out, v->valuestring);
    }
}

bool gadget_chat_parse_ack(const char *json, gadget_chat_ack_t *ack)
{
    memset(ack, 0, sizeof(*ack));
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }
    const cJSON *r = cJSON_GetObjectItem(root, "result");
    const cJSON *o = cJSON_IsObject(r) ? r : root;
    copy_id(o, "message_id", ack->message_id);
    copy_id(o, "reply_to_message_id", ack->reply_to_message_id);
    cJSON_Delete(root);
    return ack->message_id[0] != '\0' || ack->reply_to_message_id[0] != '\0';
}

void gadget_chat_init(gadget_chat_t *c)
{
    memset(c, 0, sizeof(*c));
}

static int find_msg(gadget_chat_t *c, const char *id, bool add)
{
    for (int i = 0; i < GADGET_CHAT_TRACK; i++) {
        if (c->msg[i].id[0] && strcmp(c->msg[i].id, id) == 0) {
            return i;
        }
    }
    if (!add || strlen(id) >= GADGET_CHAT_ID_MAX) {
        return -1;
    }
    int i = (int)(c->next++ % GADGET_CHAT_TRACK);
    memset(&c->msg[i], 0, sizeof(c->msg[i]));
    strcpy(c->msg[i].id, id);
    return i;
}

static const char *str_of(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

char *gadget_chat_feed(gadget_chat_t *c, const char *line, char parent[GADGET_CHAT_ID_MAX])
{
    parent[0] = '\0';
    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        return NULL;
    }
    char *out = NULL;
    const char *type = str_of(root, "type");
    const char *ev = str_of(root, "event");
    const cJSON *p = cJSON_GetObjectItem(root, "payload");
    if (type == NULL || strcmp(type, "event") != 0 || ev == NULL || !cJSON_IsObject(p)) {
        cJSON_Delete(root);
        return NULL;
    }
    const char *id = str_of(p, "message_id");
    if (id == NULL) {
        id = str_of(p, "id");
    }
    if (id == NULL) {
        id = str_of(root, "message_id");
    }
    const char *par = str_of(p, "reply_to_message_id");
    if (par == NULL) {
        par = str_of(p, "parent_message_id");
    }
    bool start = strcmp(ev, "delta.message_start") == 0;
    bool done = strcmp(ev, "delta.message_done") == 0;
    bool full = strcmp(ev, "message.assistant") == 0;
    if ((start || done || full) && id != NULL) {
        int m = find_msg(c, id, true);
        if (m >= 0 && par != NULL && strlen(par) < GADGET_CHAT_ID_MAX) {
            strcpy(c->msg[m].parent, par);
        }
        const char *text = str_of(p, "display_text");
        if (text == NULL || text[0] == '\0') {
            text = str_of(p, "content");
        }
        bool ready = !cJSON_IsFalse(cJSON_GetObjectItem(p, "display_text_ready"));
        if ((done || full) && m >= 0 && !c->msg[m].done && ready && text != NULL && text[0] != '\0') {
            size_t n = strlen(text) + 1;
            out = malloc(n);
            if (out != NULL) {
                memcpy(out, text, n);
                c->msg[m].done = true;
                strcpy(parent, c->msg[m].parent);
            }
        }
    }
    cJSON_Delete(root);
    return out;
}

bool gadget_chat_reply_is_ours(const char *parent, const gadget_chat_ack_t *ack)
{
    if (ack == NULL || parent == NULL || parent[0] == '\0') {
        return true;
    }
    return (ack->message_id[0] && strcmp(parent, ack->message_id) == 0) ||
           (ack->reply_to_message_id[0] && strcmp(parent, ack->reply_to_message_id) == 0);
}
