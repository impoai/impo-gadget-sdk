/*
 * Pairing credentials: the device access/refresh tokens issued by
 * POST /api/v1/gadgets/pairings. Kept in KV; the build-time values in
 * local.conf seed an empty store.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisa_kv.h"
#include "shell.h"

#include "link_creds.h"

#define TAG "creds"
#include "lisa_log.h"

#define KV_ACCESS  "impo.access"
#define KV_REFRESH "impo.refresh"

static volatile unsigned s_generation;

unsigned link_creds_generation(void)
{
    return s_generation;
}

static int kv_get(const char *key, char *out, size_t cap)
{
    char *v = NULL;
    out[0] = '\0';
    if (lisa_kv_get_string(key, &v) != 0 || v == NULL) {
        return -1;
    }
    int ok = strlen(v) < cap;
    if (ok) {
        strcpy(out, v);
    }
    lisa_kv_free(v);
    return ok ? 0 : -1;
}

int link_creds_load(link_creds_t *c)
{
    memset(c, 0, sizeof(*c));
    if (kv_get(KV_REFRESH, c->refresh, sizeof(c->refresh)) != 0 || c->refresh[0] == '\0') {
        if (CONFIG_GADGET_REFRESH_TOKEN[0] == '\0') {
            return -1;
        }
        snprintf(c->access, sizeof(c->access), "%s", CONFIG_GADGET_ACCESS_TOKEN);
        snprintf(c->refresh, sizeof(c->refresh), "%s", CONFIG_GADGET_REFRESH_TOKEN);
        LISA_LOGI(TAG, "using build-time pairing");
        link_creds_save(c);
        return 0;
    }
    kv_get(KV_ACCESS, c->access, sizeof(c->access));
    return 0;
}

int link_creds_save(const link_creds_t *c)
{
    int a = lisa_kv_set_string(KV_ACCESS, c->access);
    int r = lisa_kv_set_string(KV_REFRESH, c->refresh);
    s_generation++;
    return (a == 0 && r == 0) ? 0 : -1;
}

void link_creds_clear(void)
{
    lisa_kv_del(KV_ACCESS);
    lisa_kv_del(KV_REFRESH);
    s_generation++;
}

static int pair_cmd(int argc, char **argv)
{
    Shell *sh = shellGetCurrent();
    if (argc >= 4 && strcmp(argv[1], "set") == 0) {
        link_creds_t c = {0};
        snprintf(c.access, sizeof(c.access), "%s", argv[2]);
        snprintf(c.refresh, sizeof(c.refresh), "%s", argv[3]);
        shellPrint(sh, "pair set: %d\n", link_creds_save(&c));
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "clear") == 0) {
        link_creds_clear();
        shellPrint(sh, "pair cleared\n");
        return 0;
    }
    link_creds_t c;
    int have = link_creds_load(&c) == 0;
    shellPrint(sh, "paired=%d access=%u bytes refresh=%u bytes\n", have,
               (unsigned)strlen(c.access), (unsigned)strlen(c.refresh));
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 pair, pair_cmd, pair [set <access> <refresh> | clear]);
