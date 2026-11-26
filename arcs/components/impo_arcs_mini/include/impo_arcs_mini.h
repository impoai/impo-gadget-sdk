/* SPDX-License-Identifier: MIT */
#pragma once
#include "gadget_board.h" /* Official command, event and status contracts. */
#include "impo_arcs_wake.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMPO_ARCS_MINI_SDK_VERSION "0.1.0"

typedef struct {
    const char *id, *display_name, *model_id, *product_id, *instructions;
    bool continuous_dialogue;
    impo_arcs_wake_config_t wake;
} impo_arcs_mini_product_t;

/* All tables and callbacks must have static lifetime. Commands use the official
 * nonblocking gadget_command_t contract. on_init runs before audio/link start;
 * on_status runs on the SDK status task and must return promptly. */
typedef struct {
    const gadget_command_t *commands;
    int command_count;
    const gadget_event_t *events;
    int event_count;
    int (*on_init)(void);
    void (*on_status)(gadget_status_t status);
} impo_arcs_mini_application_t;

/* An optional application component supplies this function. The SDK provides
 * an empty application when IMPO_APP_DIR is empty. No custom main is needed. */
const impo_arcs_mini_application_t *impo_arcs_mini_application(void);
const impo_arcs_mini_product_t *impo_arcs_mini_product(void);

/* Task-context APIs, never ISR callbacks. Return -1 when unavailable/offline.
 * Conversation requests use the official voice state machine. */
int impo_arcs_mini_begin_conversation(void);
int impo_arcs_mini_say(const char *text);
int impo_arcs_mini_set_wake_enabled(bool enabled);
bool impo_arcs_mini_wake_available(void);
/* data ownership follows official gadget_send_event(). */
int impo_arcs_mini_send_event(const char *name, cJSON *data);

#ifdef __cplusplus
}
#endif
