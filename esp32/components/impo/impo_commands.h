/*
 * Copyright (c) Impo contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Commands a gadget with the full UI offers its account, beyond what Home Link
 * registers itself. Each is declared in link.register with a description the
 * agent reads, and runs when a link.invoke names it.
 *
 * A board adds its own with impo_board_t.commands. `required` and `optional`
 * are JSON objects of parameter name to {"type", "description"}, or NULL.
 * `run` returns a result object it owns no longer: {"ok":true,...} or
 * impo_command_error(). It runs on the Link session task, so it must not block.
 */
typedef struct {
    const char *name;
    const char *description;
    const char *required;
    const char *optional;
    cJSON *(*run)(const cJSON *params);
} impo_command_t;

/* Adds every command of this gadget to the link.register "commands_v2" object. */
void impo_commands_describe(cJSON *commands);

/* Adds the link.register "capabilities" object (what this gadget has, as
 * CAPABILITIES.md defines it, from the board and the build) and the board's
 * words to the agent: "events" and "instructions", when it has them. */
void impo_commands_capabilities(cJSON *params);

/* Runs `name` if it is one of these commands; NULL means it is not. */
cJSON *impo_commands_run(const char *name, const cJSON *params);

cJSON *impo_command_ok(void);
cJSON *impo_command_error(const char *code, const char *message);

#ifdef __cplusplus
}
#endif
