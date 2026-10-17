/*
 * impo_link_stream_* (impo_link.h) on the shared link's request streams,
 * noise_ctrl_req_open / send / cancel: POST /tts, /chat/stream and
 * /chat/subscribe on the session's own Noise connection, as upstream's
 * impo_chat_link.c does for boards whose chat rides Link's session.
 *
 * Each open stream has a slot; the callback's context is the slot and its
 * generation, so a reply frame that arrives after a cancel (upstream allows
 * that) finds a newer generation and is dropped instead of reaching the next
 * request's sink.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"

#include "sysutils.h"

#include "platform_arcs.h"
#include "noise_control.h"

#include "impo_link.h"
#include "impo_link_internal.h"

#define TAG "link_req"
#include "lisa_log.h"

#define SLOTS 4
/* Body bytes per noise_ctrl_req_send: well inside a session built for Impo on Link's
 * session (its outbound scratch is 6 KB, its request queue holds 4 KB). */
#define CHUNK_MAX 2048
#define SEND_WAIT_MS 5000

typedef struct {
    bool used;
    uint16_t gen;
    int64_t id;
    const impo_link_stream_sink_t *sink;
} slot_t;

static __psram_bss__ slot_t s_slots[SLOTS];
static SemaphoreHandle_t s_lock;

void impo_link_streams_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

static int handle_of(int i)
{
    return (int)s_slots[i].gen << 8 | i;
}

/* The slot a handle or callback context names, while it is still that request. Lock held. */
static slot_t *slot_of(int handle)
{
    int i = handle & 0xff;
    if (handle < 0 || i >= SLOTS || !s_slots[i].used || s_slots[i].gen != (uint16_t)(handle >> 8)) {
        return NULL;
    }
    return &s_slots[i];
}

static void release(slot_t *s)
{
    s->used = false;
    s->id = 0;
    s->gen++;
}

/* On the session task (noise_ctrl_req_cb). */
static void on_frame(void *ctx, int status, const uint8_t *data, size_t len, bool end)
{
    int handle = (int)(intptr_t)ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    slot_t *s = slot_of(handle);
    const impo_link_stream_sink_t *sink = s != NULL ? s->sink : NULL;
    bool done = s != NULL && (end || status < 0);
    if (done) {
        release(s);
    }
    xSemaphoreGive(s_lock);
    if (sink == NULL) {
        return;   /* cancelled: the sink hears nothing more */
    }
    if (status > 0 && sink->on_status != NULL) {
        sink->on_status(status);
    }
    if (len > 0 && sink->on_data != NULL) {
        sink->on_data(data, len);
    }
    if (done && sink->on_end != NULL) {
        sink->on_end(status == NOISE_CTRL_REQ_TOO_LARGE ? "the reply was too large for this gadget"
                     : status < 0                       ? "connection to Impo dropped"
                                                        : NULL);
    }
}

static bool send_body(int64_t id, const uint8_t *data, size_t len, bool end)
{
    do {
        size_t n = len > CHUNK_MAX ? CHUNK_MAX : len;
        if (!noise_ctrl_req_send(id, data, n, end && n == len, SEND_WAIT_MS)) {
            return false;
        }
        data += n;
        len -= n;
    } while (len > 0);
    return true;
}

int impo_link_stream_open(const char *path, const char *content_type, const char *accept, const void *body,
                          size_t len, bool more, const impo_link_stream_sink_t *sink)
{
    if (!noise_ctrl_is_connected() || path == NULL || sink == NULL) {
        return -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int handle = -1;
    for (int i = 0; i < SLOTS && handle < 0; i++) {
        if (!s_slots[i].used) {
            s_slots[i].used = true;
            s_slots[i].sink = sink;
            handle = handle_of(i);
        }
    }
    xSemaphoreGive(s_lock);
    if (handle < 0) {
        LISA_LOGE(TAG, "no free stream for %s", path);
        return -1;
    }

    /* Headers as upstream impo_chat_link.c request(). */
    char req_id[40];
    uint32_t rnd[2] = { 0, 0 };
    plat_random(rnd, sizeof(rnd));
    snprintf(req_id, sizeof(req_id), "impo-%08lx-%08lx", (unsigned long)rnd[0], (unsigned long)rnd[1]);
    const char *headers[9];
    int h = 0;
    headers[h++] = "x-request-id";
    headers[h++] = req_id;
    headers[h++] = "x-app-id";
    headers[h++] = "hatch-web";
    if (content_type != NULL) {
        headers[h++] = "Content-Type";
        headers[h++] = content_type;
    }
    if (accept != NULL) {
        headers[h++] = "Accept";
        headers[h++] = accept;
    }
    headers[h] = NULL;

    if (sink->on_begin != NULL) {
        sink->on_begin();
    }
    bool has_body = len > 0 || more;
    int64_t id = noise_ctrl_req_open("POST", path, headers, !has_body, on_frame, (void *)(intptr_t)handle);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    slot_t *s = slot_of(handle);
    if (s != NULL) {
        s->id = id;
        if (id == 0) {
            release(s);
        }
    }
    xSemaphoreGive(s_lock);
    if (id == 0) {
        LISA_LOGE(TAG, "%s: the session is down or busy", path);
        if (sink->on_end != NULL) {
            sink->on_end("could not reach Impo");
        }
        return -1;
    }
    LISA_LOGI(TAG, "stream %lld: POST %s", (long long)id, path);
    if (len > 0 && !send_body(id, body, len, !more)) {
        LISA_LOGE(TAG, "stream %lld: body not queued", (long long)id);
        impo_link_stream_cancel(handle);
        if (sink->on_end != NULL) {
            sink->on_end("could not send to Impo");
        }
        return -1;
    }
    return handle;
}

int impo_link_stream_send(int handle, const void *data, size_t len, bool end)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    slot_t *s = slot_of(handle);
    int64_t id = s != NULL ? s->id : 0;
    xSemaphoreGive(s_lock);
    if (id == 0) {
        return -1;
    }
    return send_body(id, data, len, end) ? 0 : -1;
}

void impo_link_stream_cancel(int handle)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    slot_t *s = slot_of(handle);
    int64_t id = s != NULL ? s->id : 0;
    if (s != NULL) {
        release(s);
    }
    xSemaphoreGive(s_lock);
    if (id != 0) {
        noise_ctrl_req_cancel(id);
    }
}
