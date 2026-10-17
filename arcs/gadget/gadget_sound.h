/* SDK speaker: one sound at a time from a ring of 16 kHz mono PCM. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GADGET_SOUND_RATE 16000   /* what gadget_sound_write takes: mono, 16-bit */

typedef enum {
    GADGET_SOUND_IDLE,
    GADGET_SOUND_FETCHING,   /* begun, no samples yet */
    GADGET_SOUND_DECODING,   /* downloaded, being decoded (play_url) */
    GADGET_SOUND_PLAYING,
    GADGET_SOUND_FAILED,
} gadget_sound_state_t;

/* Opens the speaker; set_amp may be NULL (amplifier always on). */
int gadget_sound_start(void (*set_amp)(bool on));

/* Starts a new sound, dropping the current one. Returns its generation for write/end. */
uint32_t gadget_sound_begin(const char *what);
/* Queues 16 kHz mono samples, waiting up to timeout_ms for room. 0 ok, -1 replaced or stopped, -2 timeout. */
int gadget_sound_write(uint32_t gen, const int16_t *pcm, size_t samples, uint32_t timeout_ms);
/* No more samples for this sound; error (or NULL) says why it ended early. */
void gadget_sound_end(uint32_t gen, const char *error);
void gadget_sound_stop(void);
void gadget_sound_set_decoding(uint32_t gen);   /* play_url: downloaded, decoding before the first samples */
size_t gadget_sound_room(void);   /* free samples in the ring */

int gadget_sound_beep(void);        /* rising: start */
int gadget_sound_beep_down(void);   /* falling: done */
int gadget_sound_volume(void);
int gadget_sound_set_volume(int percent);   /* 0..100, saved */
gadget_sound_state_t gadget_sound_state(void);
const char *gadget_sound_error(void);

#ifdef __cplusplus
}
#endif
