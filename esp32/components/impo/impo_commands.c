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

#include "impo_commands.h"

#include <string.h>

#include "impo_audio.h"
#include "impo_board.h"
#include "impo_settings.h"
#include "impo_state.h"

#define TEXT_MAX 200

cJSON *impo_command_ok(void)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    return result;
}

cJSON *impo_command_error(const char *code, const char *message)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_AddObjectToObject(result, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return result;
}

/* Wakes the screen so the person sees what the command did. */
static void wake(void)
{
    impo_state_set_asleep(false);
    impo_state_poke();
}

static cJSON *show_text(const cJSON *params)
{
    const cJSON *text = cJSON_GetObjectItem(params, "text");
    if (!cJSON_IsString(text) || !text->valuestring[0] || strlen(text->valuestring) > TEXT_MAX) {
        return impo_command_error("invalid_param", "text must be 1 to 200 characters");
    }
    wake();
    impo_state_set_caption("%s", text->valuestring);
    return impo_command_ok();
}

static cJSON *avatar_cheer(const cJSON *params)
{
    (void)params;
    wake();
    impo_state_make_happy();
    return impo_command_ok();
}

static cJSON *set_volume(const cJSON *params)
{
    const cJSON *percent = cJSON_GetObjectItem(params, "percent");
    if (!cJSON_IsNumber(percent) || percent->valuedouble < 0 || percent->valuedouble > 100) {
        return impo_command_error("invalid_param", "percent must be 0 to 100");
    }
    impo_settings_set_volume(percent->valueint);
    impo_audio_set_volume(impo_settings_volume());
    cJSON *result = impo_command_ok();
    cJSON_AddNumberToObject(result, "percent", impo_settings_volume());
    return result;
}

static cJSON *beep(const cJSON *params)
{
    (void)params;
    if (!impo_settings_speaker_on()) {
        return impo_command_error("speaker_off", "the speaker is turned off in the gadget's settings");
    }
    impo_audio_chirp(1);   /* 90 ms */
    return impo_command_ok();
}

static const impo_command_t COMMON[] = {
    { "display.show_text",
      "Show a short line of text on the gadget's screen, under the avatar, until the next thing it "
      "shows. Plain ASCII only: other characters are replaced or dropped. Wakes the screen.",
      "{\"text\":{\"type\":\"string\",\"description\":\"1 to 200 characters of ASCII text.\"}}", NULL, show_text },
    { "avatar.cheer",
      "Make the on-screen avatar hop happily for about two seconds, as it does when petted. Wakes the screen.",
      NULL, NULL, avatar_cheer },
    { "speaker.set_volume",
      "Set the gadget's speaker volume and remember it.",
      "{\"percent\":{\"type\":\"integer\",\"description\":\"0 (silent) to 100.\"}}", NULL, set_volume },
    { "speaker.beep",
      "Play one short chirp on the gadget's speaker, to get attention or to check the speaker works.",
      NULL, NULL, beep },
};

static void describe(cJSON *commands, const impo_command_t *command)
{
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "description", command->description);
    cJSON *required = command->required ? cJSON_Parse(command->required) : NULL;
    cJSON *optional = command->optional ? cJSON_Parse(command->optional) : NULL;
    cJSON_AddItemToObject(entry, "required", required ? required : cJSON_CreateObject());
    cJSON_AddItemToObject(entry, "optional", optional ? optional : cJSON_CreateObject());
    cJSON_AddItemToObject(commands, command->name, entry);
}

void impo_commands_describe(cJSON *commands)
{
    for (size_t i = 0; i < sizeof(COMMON) / sizeof(COMMON[0]); i++) {
        describe(commands, &COMMON[i]);
    }
    for (int i = 0; impo_board && i < impo_board->command_count; i++) {
        describe(commands, &impo_board->commands[i]);
    }
}

/* Link sends back only "ok", "error" and "payload": whatever else a command
 * put in its result (a reading, a state) is gathered under "payload". */
static cJSON *wrap(cJSON *result)
{
    if (!result || cJSON_GetObjectItem(result, "payload")) {
        return result;
    }
    cJSON *payload = NULL;
    cJSON *item = result->child;
    while (item) {
        cJSON *next = item->next;
        if (strcmp(item->string, "ok") && strcmp(item->string, "error")) {
            if (!payload) {
                payload = cJSON_CreateObject();
            }
            cJSON_AddItemToObject(payload, item->string, cJSON_DetachItemViaPointer(result, item));
        }
        item = next;
    }
    if (payload) {
        cJSON_AddItemToObject(result, "payload", payload);
    }
    return result;
}

cJSON *impo_commands_run(const char *name, const cJSON *params)
{
    for (size_t i = 0; i < sizeof(COMMON) / sizeof(COMMON[0]); i++) {
        if (!strcmp(name, COMMON[i].name)) {
            return wrap(COMMON[i].run(params));
        }
    }
    for (int i = 0; impo_board && i < impo_board->command_count; i++) {
        if (!strcmp(name, impo_board->commands[i].name)) {
            return wrap(impo_board->commands[i].run(params));
        }
    }
    return NULL;
}
