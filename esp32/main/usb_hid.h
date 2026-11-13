// SPDX-License-Identifier: MIT
// A USB keyboard and mouse on the chip's own USB port
// (CONFIG_HOMEHUB_USB_HID): keyboard.type and mouse.move, on the computer the
// gadget is plugged into. See usb_hid.c for how the port is shared with the
// serial console.
#pragma once

#include "cJSON.h"
#include "noise_control.h"

#ifdef __cplusplus
extern "C" {
#endif

// Runs keyboard.type or mouse.move. Checks the parameters, queues the input
// and returns {"_async": true}; the result follows through
// noise_ctrl_send_command_result() once the input has been sent. Returns an
// error result for wrong parameters or a full queue, and NULL when `command`
// is neither. Returns at once.
cJSON *usb_hid_command(const char *command, const cJSON *params,
                       const char *request_id,
                       noise_ctrl_session_generation_t session_generation);

#ifdef __cplusplus
}
#endif
