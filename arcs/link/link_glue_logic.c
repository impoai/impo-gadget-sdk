/* Pure decisions of the link glue; see link_glue_logic.h. */
#include <stdio.h>
#include <string.h>

#include "link_glue_logic.h"

bool link_product_id_ok(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return true;
    }
    /* A lowercase UUID: 8-4-4-4-12 hex digits. */
    if (strlen(id) != 36) {
        return false;
    }
    for (size_t i = 0; i < 36; i++) {
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? id[i] != '-' : strchr("0123456789abcdef", id[i]) == NULL) {
            return false;
        }
    }
    return true;
}

uint32_t link_auth_refresh_interval_ms(unsigned refusals)
{
    unsigned doublings = refusals > 0 ? refusals - 1 : 0;
    if (doublings > 5) {
        doublings = 5;
    }
    uint32_t ms = 60000u << doublings;
    return ms > 30u * 60000u ? 30u * 60000u : ms;
}

uint32_t link_retry_next_ms(uint32_t current_ms)
{
    if (current_ms < LINK_RETRY_MIN_MS) {
        return LINK_RETRY_MIN_MS;
    }
    return current_ms >= LINK_RETRY_MAX_MS / 2 ? LINK_RETRY_MAX_MS : current_ms * 2;
}

void link_result_to_upstream(cJSON *result)
{
    cJSON *error = cJSON_GetObjectItem(result, "error");
    if (!cJSON_IsString(error)) {
        return;
    }
    const char *msg = error->valuestring;
    const char *code = strcmp(msg, "unknown command") == 0 ? "unsupported"
                       : strchr(msg, '"') != NULL ? "invalid_param" : "failed";
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    cJSON_ReplaceItemInObject(result, "error", e);
}

void link_history_add(link_history_t *h, uint32_t at_s, uint32_t online_s, uint32_t quiet_ms, uint32_t invokes,
                      const char *why)
{
    link_session_end_t *e = &h->ends[h->n % LINK_HISTORY];
    e->at_s = at_s;
    e->online_s = online_s;
    e->quiet_ms = quiet_ms;
    e->invokes = invokes;
    snprintf(e->why, sizeof(e->why), "%s", why != NULL ? why : "?");
    h->n++;
}

int link_history_format(const link_history_t *h, char *buf, size_t cap)
{
    size_t w = 0;
    if (cap > 0) {
        buf[0] = '\0';
    }
    unsigned first = h->n > LINK_HISTORY ? h->n - LINK_HISTORY : 0;
    for (unsigned i = first; i < h->n && w < cap; i++) {
        const link_session_end_t *e = &h->ends[i % LINK_HISTORY];
        int n = snprintf(buf + w, cap - w,
                         "\n  ended at %us after %us online: %s (last rx %u ms before, %u invokes so far)",
                         (unsigned)e->at_s, (unsigned)e->online_s, e->why, (unsigned)e->quiet_ms,
                         (unsigned)e->invokes);
        if (n < 0) {
            break;
        }
        w += (size_t)n < cap - w ? (size_t)n : cap - w - 1;
    }
    return (int)w;
}
