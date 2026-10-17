/* L1: connection to the Impo gadget gateway. Hardware-independent. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IMPO_LINK_IDLE,
    IMPO_LINK_UNPAIRED,
    IMPO_LINK_FETCHING,
    IMPO_LINK_CONNECTING,
    IMPO_LINK_REGISTERING,
    IMPO_LINK_ONLINE,
} impo_link_state_t;

/* Start the link task. It waits for Wi-Fi and the clock, then keeps the device online. */
int impo_link_start(void);
impo_link_state_t impo_link_state(void);
int impo_link_describe(char *buf, size_t cap);

/* Queue a link.event for the agent; takes ownership of data (may be NULL). -1 when offline. */
struct cJSON;
int impo_link_send_event(const char *name, struct cJSON *data);
/* Send link.register again (name or capabilities changed). */
void impo_link_request_reregister(void);

/* Receives one request stream's reply, on the link task. on_begin runs when the request
 * goes out; on_end gets NULL when the body ended normally, else why it failed. */
typedef struct {
    void (*on_begin)(void);
    void (*on_status)(int status);
    void (*on_data)(const unsigned char *data, size_t len);
    void (*on_end)(const char *error);
} impo_link_stream_sink_t;

/* Request streams on the Noise session. Any task may call these; the work runs on the link
 * task in order, and the sink's callbacks run there too.
 * open: POST path with body; more=true when send() will add to the body. Returns a handle, or
 * -1 when offline or all streams are busy. send: more body; end=true closes the request body.
 * cancel: resets the stream; the sink hears nothing more. */
int impo_link_stream_open(const char *path, const char *content_type, const char *accept, const void *body,
                          size_t len, bool more, const impo_link_stream_sink_t *sink);
int impo_link_stream_send(int handle, const void *data, size_t len, bool end);
void impo_link_stream_cancel(int handle);

/* Debug: ends the current session as if the network had dropped it. */
void impo_link_debug_drop(void);

#ifdef __cplusplus
}
#endif
