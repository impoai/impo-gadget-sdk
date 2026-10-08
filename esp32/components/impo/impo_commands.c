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
#include "impo_chat.h"
#include "impo_settings.h"
#include "impo_state.h"
#include "impo_voice.h"

static const char *TAG = "impo_commands";

#define TEXT_MAX 200
#define SOUND_MAX_BYTES (1536 * 1024)   /* an MP3 of a minute at 192 kbps */
#define SOUND_MAX_SECONDS 60
#define SOUND_TIMEOUT_MS 15000

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

/*
 * speaker.play_url: the MP3 is fetched and decoded on a task of its own,
 * then handed to the voice loop; the command returns at once and
 * speaker.status says how it went. One sound at a time.
 */
static struct {
    volatile bool busy;           /* fetching or decoding */
    char state[16];               /* "idle", "fetching", "decoding", "playing", "failed" */
    char error[64];
} s_sound = { .state = "idle" };

static void sound_set(const char *state, const char *error)
{
    strlcpy(s_sound.state, state, sizeof(s_sound.state));
    strlcpy(s_sound.error, error ? error : "", sizeof(s_sound.error));
}

/* Fetches `url` into PSRAM, up to SOUND_MAX_BYTES; the caller frees *out. */
static esp_err_t fetch(const char *url, uint8_t **out, size_t *len, const char **error)
{
    *out = NULL;
    *len = 0;
    const esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = SOUND_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) {
        *error = "out of memory";
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_open(http, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(http);
        int status = esp_http_client_get_status_code(http);
        int64_t announced = esp_http_client_get_content_length(http);
        if (status != 200) {
            *error = "the URL did not answer with the sound";
            err = ESP_FAIL;
        } else if (announced > SOUND_MAX_BYTES) {
            *error = "the sound is too large";
            err = ESP_ERR_INVALID_SIZE;
        }
    } else {
        *error = "the URL could not be reached";
    }
    size_t cap = SOUND_MAX_BYTES, have = 0;
    uint8_t *buf = err == ESP_OK ? heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (err == ESP_OK && !buf) {
        *error = "out of memory";
        err = ESP_ERR_NO_MEM;
    }
    while (err == ESP_OK) {
        int n = esp_http_client_read(http, (char *)buf + have, (int)(cap - have));
        if (n < 0) {
            *error = "the download broke off";
            err = ESP_FAIL;
        } else if (n == 0) {
            break;
        } else if ((have += n) == cap && !esp_http_client_is_complete_data_received(http)) {
            *error = "the sound is too large";
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    esp_http_client_close(http);
    esp_http_client_cleanup(http);
    if (err != ESP_OK || !have) {
        free(buf);
        if (err == ESP_OK) {
            *error = "the sound is empty";
            err = ESP_FAIL;
        }
        return err;
    }
    *out = buf;
    *len = have;
    return ESP_OK;
}

static void sound_task(void *arg)
{
    char *url = arg;
    uint8_t *mp3 = NULL;
    size_t len = 0;
    const char *error = NULL;
    sound_set("fetching", NULL);
    esp_err_t err = fetch(url, &mp3, &len, &error);
    if (err == ESP_OK) {
        sound_set("decoding", NULL);
        int16_t *pcm = NULL;
        size_t n = impo_hatch_mp3_decode(mp3, len, SOUND_MAX_SECONDS, &pcm);
        free(mp3);
        if (n) {
            ESP_LOGI(TAG, "playing %u bytes of MP3, %.1f s", (unsigned)len, n / 16000.0);
            sound_set("playing", NULL);
            impo_state_poke();
            impo_voice_play(pcm, n);
        } else {
            error = "the sound is not an MP3 this gadget can decode";
            err = ESP_FAIL;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", error, url);
        sound_set("failed", error);
    }
    free(url);
    s_sound.busy = false;
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
    if (s_sound.busy) {
        return impo_command_error("busy", "another sound is still being fetched");
    }
    char *copy = strdup(url->valuestring);
    if (!copy) {
        return impo_command_error("out_of_memory", "no memory for the request");
    }
    s_sound.busy = true;
    if (xTaskCreateWithCaps(sound_task, "sound", 6144, copy, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_sound.busy = false;
        free(copy);
        return impo_command_error("out_of_memory", "no memory for the request");
    }
    cJSON *result = impo_command_ok();
    cJSON_AddStringToObject(result, "status", "fetching");
    cJSON_AddStringToObject(result, "note", "the sound plays once fetched and decoded; speaker.status reports progress");
    return result;
}

static cJSON *speaker_stop(const cJSON *params)
{
    (void)params;
    impo_voice_stop();
    return impo_command_ok();
}

static cJSON *speaker_status(const cJSON *params)
{
    (void)params;
    if (!strcmp(s_sound.state, "playing") && !impo_voice_playing()) {
        sound_set("idle", NULL);
    }
    cJSON *result = impo_command_ok();
    cJSON_AddStringToObject(result, "status", s_sound.state);
    if (s_sound.error[0]) {
        cJSON_AddStringToObject(result, "error", s_sound.error);
    }
    cJSON_AddNumberToObject(result, "volume_percent", impo_settings_volume());
    cJSON_AddBoolToObject(result, "speaker_on", impo_settings_speaker_on());
    return result;
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
    { "speaker.play_url",
      "Play a sound on the gadget's speaker from a public https URL of an MP3 file (music, a sound "
      "effect, speech), up to about a minute of it. The gadget fetches the file itself, so the URL "
      "must be reachable without a login. Returns at once; the sound starts a few seconds later.",
      "{\"url\":{\"type\":\"string\",\"description\":\"https URL of an MP3 file, up to 1.5 MB.\"}}",
      NULL, play_url },
    { "speaker.stop", "Stop the sound speaker.play_url is playing.", NULL, NULL, speaker_stop },
    { "speaker.status",
      "Whether the speaker is on, its volume, and what became of the last speaker.play_url: "
      "fetching, decoding, playing, idle or failed (with the reason).",
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
