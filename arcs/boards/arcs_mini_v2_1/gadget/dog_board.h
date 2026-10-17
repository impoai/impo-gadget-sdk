/* Robot dog (ARCS-MINI V2.1) board helpers. */
#pragma once

#include <stdint.h>

#include "gadget_board.h"

int dog_power_init(void);
int dog_read_power(gadget_power_t *out);
int dog_power_off(void);
unsigned dog_poll_buttons(void);

extern const gadget_command_t dog_commands[];
extern const int dog_command_count;
extern const gadget_event_t dog_events[];
extern const int dog_event_count;
int dog_commands_init(void);   /* command worker and IMU event hooks */

/* The light strip (dog_light.c): SDK status, or an effect the agent chose. */
int dog_light_start(void);
void dog_show_status(gadget_status_t st);
void dog_show_animation(void);
int dog_light_set(const char *effect, uint8_t r, uint8_t g, uint8_t b, int brightness);   /* -1: unknown effect */
