/* Pairing credentials for the Impo gadget gateway. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char access[512];
    char refresh[512];
} link_creds_t;

/* 0 when a pairing exists (KV first, then the build-time seed). */
int link_creds_load(link_creds_t *c);
int link_creds_save(const link_creds_t *c);
void link_creds_clear(void);
/* Bumped on every save or clear, so the link task can wait without polling KV. */
unsigned link_creds_generation(void);

#ifdef __cplusplus
}
#endif
