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

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "impo_audio.h"
#include "impo_board.h"
#include "impo_camera.h"
#include "impo_chat.h"
#include "impo_settings.h"
#include "impo_sound.h"
#include "impo_state.h"

static const char *TAG = "impo_commands";

#define TEXT_MAX 200
#define SOUND_TIMEOUT_MS 15000
#define SAY_MAX_CHARS 1000

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
        return impo_command_error("invalid_param", "text must be 1 to 200 bytes of UTF-8");
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

/*
 * speaker.play_url: a task of its own streams the file into impo_sound as it
 * downloads, so the memory is the module's fixed buffers whatever the
 * file's length; the decoder plays it as it comes. The command returns at
 * once and speaker.status says how it goes.
 */
#define SOUND_MAX_BYTES (1536 * 1024)   /* an MP3 of a minute at 192 kbps */
#define SOUND_WRITE_WAIT_MS 20000       /* for room: the decoder frees it as it plays */

static volatile bool s_fetching;
static char s_last_url[128];   /* what speaker.play_url last tried, for speaker.status */

static void sound_task(void *arg)
{
    char *url = arg;
    const char *error = NULL;
    size_t total = 0;
    uint8_t *chunk = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = SOUND_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t http = chunk ? esp_http_client_init(&cfg) : NULL;
    esp_err_t err = http ? esp_http_client_open(http, 0) : ESP_ERR_NO_MEM;
    if (!http) {
        error = "out of memory";
    } else if (err != ESP_OK) {
        error = "the URL could not be reached";
    } else {
        esp_http_client_fetch_headers(http);
        int status = esp_http_client_get_status_code(http);
        int64_t announced = esp_http_client_get_content_length(http);
        if (status != 200) {
            error = "the URL did not answer with the sound";
        } else if (announced > SOUND_MAX_BYTES) {
            error = "the sound is too large";
        }
    }
    impo_sound_begin("url");
    while (!error) {
        int n = esp_http_client_read(http, (char *)chunk, 4096);
        if (n < 0) {
            error = "the download broke off";
        } else if (n == 0) {
            break;
        } else if ((total += n) > SOUND_MAX_BYTES) {
            error = "the sound is too large";
        } else if (!impo_sound_write(chunk, n, SOUND_WRITE_WAIT_MS)) {
            char why[64];
            error = strcmp(impo_sound_status(why, sizeof(why)), "failed") ? "stopped" : "the sound could not be decoded";
        }
    }
    if (http) {
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
    }
    free(chunk);
    if (!error && !total) {
        error = "the sound is empty";
    }
    if (error) {
        ESP_LOGW(TAG, "%s: %s", error, url);
        if (strcmp(error, "stopped")) {
            impo_sound_fail(error);
        }
    } else {
        ESP_LOGI(TAG, "fetched %u bytes of sound", (unsigned)total);
        impo_sound_end(true);
    }
    free(url);
    s_fetching = false;
    vTaskDeleteWithCaps(NULL);
}

static cJSON *play_url(const cJSON *params)
{
    const cJSON *url = cJSON_GetObjectItem(params, "url");
    if (!cJSON_IsString(url) || strncmp(url->valuestring, "https://", 8) || strlen(url->valuestring) > 512) {
        return impo_command_error("invalid_param", "url must be an https URL of up to 512 characters");
    }
    if (!impo_settings_speaker_on()) {
        return impo_command_error("speaker_off", "the speaker is turned off in the gadget's settings");
    }
    if (s_fetching) {
        return impo_command_error("busy", "another sound is still being fetched");
    }
    char *copy = strdup(url->valuestring);
    if (!copy) {
        return impo_command_error("out_of_memory", "no memory for the request");
    }
    s_fetching = true;
    strlcpy(s_last_url, copy, sizeof(s_last_url));
    if (xTaskCreateWithCaps(sound_task, "sound_url", 6144, copy, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_fetching = false;
        free(copy);
        return impo_command_error("out_of_memory", "no memory for the request");
    }
    cJSON *result = impo_command_ok();
    cJSON_AddStringToObject(result, "status", "fetching");
    cJSON_AddStringToObject(result, "note", "the sound plays as it arrives; speaker.status reports progress");
    return result;
}

static cJSON *speaker_say(const cJSON *params)
{
    const cJSON *text = cJSON_GetObjectItem(params, "text");
    if (!cJSON_IsString(text) || !text->valuestring[0] || strlen(text->valuestring) > SAY_MAX_CHARS) {
        return impo_command_error("invalid_param", "text must be 1 to 1000 characters");
    }
    if (!impo_settings_speaker_on()) {
        return impo_command_error("speaker_off", "the speaker is turned off in the gadget's settings");
    }
    if (!impo_hatch_say(text->valuestring)) {
        return impo_command_error("unavailable", "the gadget is not connected to Impo's voice");
    }
    impo_state_poke();
    cJSON *result = impo_command_ok();
    cJSON_AddStringToObject(result, "status", "fetching");
    cJSON_AddStringToObject(result, "note", "the speech plays once Impo has made it, a few seconds from now");
    return result;
}

static cJSON *speaker_stop(const cJSON *params)
{
    (void)params;
    impo_sound_stop();
    return impo_command_ok();
}

static cJSON *speaker_status(const cJSON *params)
{
    (void)params;
    char error[64];
    const char *status = impo_sound_status(error, sizeof(error));
    cJSON *result = impo_command_ok();
    cJSON_AddStringToObject(result, "status", status);
    if (error[0]) {
        cJSON_AddStringToObject(result, "error", error);
    }
    if (s_last_url[0]) {
        cJSON_AddStringToObject(result, "last_url", s_last_url);
    }
    cJSON_AddNumberToObject(result, "volume_percent", impo_settings_volume());
    cJSON_AddBoolToObject(result, "speaker_on", impo_settings_speaker_on());
    return result;
}

static const impo_command_t COMMON[] = {
    { "display.show_text",
      "Show a short line of text on the gadget's screen, under the avatar, until the next thing it "
      "shows. UTF-8; Chinese and Japanese show on gadgets built with the CJK font, as boxes on "
      "others. Wakes the screen.",
      "{\"text\":{\"type\":\"string\",\"description\":\"The text, up to 200 bytes (about 60 Chinese characters).\"}}", NULL, show_text },
    { "avatar.cheer",
      "Make the on-screen avatar hop happily for about two seconds, as it does when petted. Wakes the screen.",
      NULL, NULL, avatar_cheer },
    { "speaker.set_volume",
      "Set the gadget's speaker volume and remember it.",
      "{\"percent\":{\"type\":\"integer\",\"description\":\"0 (silent) to 100.\"}}", NULL, set_volume },
    { "speaker.beep",
      "Play one short chirp on the gadget's speaker, to get attention or to check the speaker works.",
      NULL, NULL, beep },
    { "speaker.play_url",
      "Play a sound on the gadget's speaker from a public https URL that points straight at an MP3 "
      "file (music, a sound effect, speech), up to about a minute of it. The gadget fetches the file "
      "itself with no login and no browser, so a web page, a streaming service or a player link won't "
      "do: the response must be the audio bytes. Returns at once; the sound starts within a couple of "
      "seconds, and speaker.status says whether it played or why not.",
      "{\"url\":{\"type\":\"string\",\"description\":\"https URL of an MP3 file, up to 1.5 MB.\"}}",
      NULL, play_url },
    { "speaker.say",
      "Say something out loud on the gadget's speaker, in Impo's voice: a sentence or two for the "
      "person next to it. Returns at once; the speech starts a few seconds later.",
      "{\"text\":{\"type\":\"string\",\"description\":\"What to say, 1 to 1000 characters, any language.\"}}",
      NULL, speaker_say },
    { "speaker.stop", "Stop the sound speaker.say or speaker.play_url is playing.", NULL, NULL, speaker_stop },
    { "speaker.status",
      "Whether the speaker is on, its volume, and what became of the last speaker.say or "
      "speaker.play_url: fetching, playing, idle or failed (with the reason).",
      NULL, NULL, speaker_status },
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

void impo_commands_capabilities(cJSON *params)
{
    cJSON *caps = cJSON_AddObjectToObject(params, "capabilities");
    if (!caps || !impo_board) {
        return;
    }
    cJSON *screen = cJSON_AddObjectToObject(caps, "screen");
    cJSON_AddNumberToObject(screen, "width", impo_board->width);
    cJSON_AddNumberToObject(screen, "height", impo_board->height);
    cJSON_AddBoolToObject(screen, "color", true);
    cJSON_AddBoolToObject(screen, "round", impo_board->round);
    cJSON_AddBoolToObject(screen, "touch", impo_board->touch);
    cJSON_AddBoolToObject(caps, "avatar", true);
    cJSON_AddBoolToObject(caps, "speaker", impo_board->audio_init != NULL);
    cJSON_AddBoolToObject(caps, "microphone", impo_board->audio_init != NULL);
    cJSON_AddBoolToObject(caps, "push_to_talk", impo_board->audio_init != NULL);
    cJSON_AddBoolToObject(caps, "camera", impo_camera_name() != NULL);
    cJSON_AddBoolToObject(caps, "battery", impo_board->read_power != NULL);
    cJSON_AddBoolToObject(caps, "buttons", impo_board->poll_buttons != NULL);
    cJSON *features = cJSON_AddArrayToObject(caps, "features");
    const char *list = impo_board->features;
    while (list && *list) {
        const char *end = strchr(list, ',');
        size_t len = end ? (size_t)(end - list) : strlen(list);
        if (len) {
            char name[32];
            snprintf(name, sizeof(name), "%.*s", (int)len, list);
            cJSON_AddItemToArray(features, cJSON_CreateString(name));
        }
        list = end ? end + 1 : list + len;
    }
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
