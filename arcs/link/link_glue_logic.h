/* Pure decisions of the link glue (impo_link_glue.c), kept apart for the host tests. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A product id as contracts/gadget-identity.md allows it: the lowercase UUID Impo assigned.
 * NULL and "" are "not set up" and also pass; anything else is refused. */
bool link_product_id_ok(const char *id);

/* Floor between VM credential refreshes after `refusals` refusals in a row of
 * freshly fetched credentials: 1, 2, 4, 8, 16 min, then 30 min
 * (adapted from upstream esp32/main/app.c note_vm_credentials_refused, 8f07ace). */
uint32_t link_auth_refresh_interval_ms(unsigned refusals);

/* The next wait before trying to start a session again: 5 s doubling to 60 s
 * (upstream app.c IMPO_VM_RETRY_MIN_US / IMPO_VM_RETRY_MAX_US). */
uint32_t link_retry_next_ms(uint32_t current_ms);
#define LINK_RETRY_MIN_MS 5000u
#define LINK_RETRY_MAX_MS 60000u

/* gadget_run_command() answers {"ok":false,"error":"why"}; upstream's
 * link.result takes the error's text only from {"error":{"code","message"}}.
 * Rewrites the first into the second (code "unsupported" for an unknown
 * command, "invalid_param" for a refused parameter, else "failed"). */
void link_result_to_upstream(cJSON *result);

/* The last session ends, for `link`. */
#define LINK_HISTORY 10
typedef struct {
    uint32_t at_s, online_s, quiet_ms, invokes;
    char why[40];
} link_session_end_t;

typedef struct {
    link_session_end_t ends[LINK_HISTORY];
    unsigned n;
} link_history_t;

void link_history_add(link_history_t *h, uint32_t at_s, uint32_t online_s, uint32_t quiet_ms, uint32_t invokes,
                      const char *why);
/* One line per end, oldest first, each starting "\n  ended at". Returns the length written. */
int link_history_format(const link_history_t *h, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
