/* SDK status light and battery task (gadget_policy decides, the board shows). */
#pragma once

#include <stdbool.h>

#include "gadget_board.h"

#ifdef __cplusplus
extern "C" {
#endif

int gadget_status_start(bool voice);
/* Latest battery reading (read once a second); false before the first one or without a battery. */
bool gadget_status_power(gadget_power_t *out);
gadget_status_t gadget_status_current(void);
/* display.show_animation: the board's light goes back to showing the status. */
void gadget_status_show_again(void);

#ifdef __cplusplus
}
#endif
