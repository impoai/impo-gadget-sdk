/* SDK core internals used by the link and app; boards use gadget_board.h. */
#pragma once

#include "cJSON.h"
#include "gadget_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runs the board's init. Call after plat_init(), before the link starts. */
int gadget_start(void);

/* Button policy task (long press powers off). */
int gadget_input_start(void);
/* Called on the input task when the AUX button goes down (the voice trigger). */
void gadget_input_on_aux(void (*cb)(void));

/* The stored name (device.set_name), else the board's. */
const char *gadget_display_name(void);

/* link.register: every command into commands_v2 (names already there are kept), and "capabilities". */
void gadget_describe_commands(cJSON *commands);
void gadget_fill_capabilities(cJSON *params);

/* Runs a standard or board command. Returns {"ok":true,"payload"?} or {"ok":false,"error"}. */
cJSON *gadget_run_command(const char *name, const cJSON *params);

#ifdef __cplusplus
}
#endif
