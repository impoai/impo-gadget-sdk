/* Voice turns: trigger, listen until a pause, send to Impo, speak the reply. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GADGET_VOICE_IDLE,
    GADGET_VOICE_LISTENING,
    GADGET_VOICE_THINKING,    /* sent, waiting for the agent */
    GADGET_VOICE_SPEAKING,
} gadget_voice_state_t;

int gadget_voice_start(void);
/* Idle: start listening. Listening: stop now. Replying: interrupt and listen again. Any task. */
void gadget_voice_trigger(void);
gadget_voice_state_t gadget_voice_state(void);

/* Self-test: one turn in which the gadget speaks `text` (via /tts) into its own microphone. */
int gadget_voice_test(const char *text);
void gadget_voice_counts(uint32_t *turns, uint32_t *replies_spoken);

#ifdef __cplusplus
}
#endif
