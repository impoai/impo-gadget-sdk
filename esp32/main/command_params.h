// SPDX-License-Identifier: MIT
/*
 * Checks a command's parameters against the schema the command publishes to
 * the agent (its `required` and `optional` objects in link.register), before
 * the command runs.
 * The schema is the one source of truth: what the agent is told is what is
 * checked, so a wrong call gets an error that says what is expected instead of
 * a silent default.
 *
 * Each schema is a JSON object {name: {...}} with, per parameter:
 *   "type": "string" | "integer" | "number" | "boolean" | "object" | "array"
 *   "enum": [allowed strings]          (optional)
 *   "minimum", "maximum": numbers      (optional, for integer and number)
 *   "minLength", "maxLength": integers (optional, for string, in characters)
 *   "description": what the agent reads (quoted back in errors)
 * Unknown parameters are refused. Pure C, host-tested
 * (tests/test_freenove_4wd_car.py). The same checker as
 * arcs/gadget/gadget_params.c, so both platforms answer a wrong call alike.
 */
#ifndef COMMAND_PARAMS_H
#define COMMAND_PARAMS_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

/* true when params fit; otherwise false and err holds a sentence for the agent. */
bool command_params_check(const char *required, const char *optional, const cJSON *params, char *err, size_t cap);

#endif
