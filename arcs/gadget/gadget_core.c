/*
 * SDK core: turns the board into link.register fields, runs link.invoke
 * commands (standard ones first, then the board's), and the standard device.*
 * commands every gadget has (CAPABILITIES.md).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "esp_heap_caps.h"
#include "lisa_kv.h"
#include "lisa_time.h"

#include "platform_arcs.h"
#include "gadget_board.h"
#include "gadget_core.h"
#include "gadget_params.h"
#include "gadget_sound.h"
#include "gadget_speech.h"
#include "gadget_voice.h"
#include "gadget_status.h"
#include "gadget_policy.h"
#include "gadget_play_url.h"
#include "impo_link.h"

#define TAG "gadget"
#include "lisa_log.h"

#define KV_NAME "gadget.name"
static bool s_speaker;   /* the board has one and it started */
static bool s_voice;     /* microphone and a trigger: touch/press to talk */

/* ---- results ---- */

cJSON *gadget_command_ok(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddTrueToObject(r, "ok");
    return r;
}

cJSON *gadget_command_ok_payload(cJSON *payload)
{
    cJSON *r = gadget_command_ok();
    cJSON_AddItemToObject(r, "payload", payload ? payload : cJSON_CreateObject());
    return r;
}

cJSON *gadget_command_error(const char *message)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddFalseToObject(r, "ok");
    cJSON_AddStringToObject(r, "error", message);
    return r;
}

/* ---- device name ---- */

static char s_name[33];

const char *gadget_display_name(void)
{
    if (s_name[0] == '\0') {
        char *v = NULL;
        if (lisa_kv_get_string(KV_NAME, &v) == 0 && v != NULL && v[0] != '\0' && strlen(v) < sizeof(s_name)) {
            strcpy(s_name, v);
        } else {
            snprintf(s_name, sizeof(s_name), "%s", gadget_board_get()->name);
        }
        if (v != NULL) {
            lisa_kv_free(v);
        }
    }
    return s_name;
}

/* ---- standard commands ---- */

static cJSON *cmd_health(const cJSON *params)
{
    (void)params;
    const gadget_board_t *b = gadget_board_get();
    cJSON *p = cJSON_CreateObject();
    cJSON *m = cJSON_AddObjectToObject(p, "metrics");
    /* Field names as impo-gadget-sdk's device.health (noise_control.cpp). */
    cJSON_AddNumberToObject(m, "uptime_seconds", (double)(plat_uptime_ms() / 1000));
    cJSON_AddNumberToObject(m, "heap_free_internal", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(m, "heap_free_psram", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(m, "heap_largest_internal", (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(m, "heap_largest_psram", (double)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    const char *overall = "ok";
    gadget_power_t pw = {0};
    bool have = b->read_power != NULL && (gadget_status_power(&pw) || b->read_power(&pw) == 0);
    if (have && pw.has_battery) {
        cJSON_AddNumberToObject(m, "battery_pct", pw.battery_pct);
        cJSON_AddNumberToObject(m, "battery_mv", pw.battery_mv);
        cJSON_AddBoolToObject(m, "charging", pw.charging);
        if (pw.battery_pct <= GADGET_BATTERY_WARN_PCT && !pw.usb_power && !pw.charging) {
            overall = "low_battery";
        }
    } else {
        cJSON_AddNullToObject(m, "battery_pct");
        cJSON_AddNullToObject(m, "battery_mv");
        cJSON_AddNullToObject(m, "charging");
    }
    if (have) {
        cJSON_AddBoolToObject(m, "usb_power", pw.usb_power);
    } else {
        cJSON_AddNullToObject(m, "usb_power");
    }
    char ssid[33] = "";
    plat_wifi_get_ssid(ssid, sizeof(ssid));
    cJSON_AddStringToObject(p, "overall", overall);
    if (ssid[0] != '\0') {
        cJSON_AddStringToObject(p, "network_ssid", ssid);
    }
    cJSON_AddStringToObject(p, "version", plat_firmware_version());
    return gadget_command_ok_payload(p);
}

static cJSON *cmd_set_name(const cJSON *params)
{
    const cJSON *n = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(n) || n->valuestring[0] == '\0' || strlen(n->valuestring) > 32) {
        return gadget_command_error("name must be 1 to 32 characters");
    }
    if (lisa_kv_set_string(KV_NAME, n->valuestring) != 0) {
        return gadget_command_error("could not store the name");
    }
    strcpy(s_name, n->valuestring);
    impo_link_request_reregister();
    return gadget_command_ok();
}

static const gadget_command_t s_standard[] = {
    { "device.health",
      "How the gadget is doing: overall state, uptime, free memory, battery level and charging, "
      "the Wi-Fi network it is on, and its firmware version.",
      NULL, NULL, cmd_health },
    { "device.set_name",
      "Rename the gadget. It reconnects under the new name and the gadget list shows it from then on.",
      "{\"name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":32,\"description\":\"The new name, 1 to 32 characters.\"}}",
      NULL, cmd_set_name },
};

#define STANDARD_COUNT (int)(sizeof(s_standard) / sizeof(s_standard[0]))

/* ---- speaker.* (CAPABILITIES.md), registered when the board has a speaker ---- */

static const char *state_name(gadget_sound_state_t s)
{
    switch (s) {
    case GADGET_SOUND_FETCHING: return "fetching";
    case GADGET_SOUND_DECODING: return "decoding";
    case GADGET_SOUND_PLAYING: return "playing";
    case GADGET_SOUND_FAILED: return "failed";
    default: return "idle";
    }
}

static cJSON *cmd_beep(const cJSON *params)
{
    (void)params;
    return gadget_sound_beep() == 0 ? gadget_command_ok() : gadget_command_error("speaker busy");
}

#define SAY_MAX 1000

static cJSON *cmd_say(const cJSON *params)
{
    const cJSON *t = cJSON_GetObjectItem(params, "text");
    if (!cJSON_IsString(t) || t->valuestring[0] == '\0' || strlen(t->valuestring) > SAY_MAX) {
        return gadget_command_error("text must be 1 to 1000 characters");
    }
    if (gadget_speech_say(t->valuestring) != 0) {
        return gadget_command_error("not connected to Impo");
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "fetching");
    cJSON_AddStringToObject(r, "note", "the speech plays once Impo has made it, a few seconds from now");
    return gadget_command_ok_payload(r);
}

static cJSON *cmd_play_url(const cJSON *params)
{
    const cJSON *u = cJSON_GetObjectItem(params, "url");
    const char *why = "url is required";
    if (!cJSON_IsString(u) || gadget_play_url(u->valuestring, &why) != 0) {
        return gadget_command_error(why);
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "fetching");
    cJSON_AddStringToObject(r, "note", "it plays once downloaded; speaker.status tells how it went");
    return gadget_command_ok_payload(r);
}

static cJSON *cmd_set_volume(const cJSON *params)
{
    const cJSON *p = cJSON_GetObjectItem(params, "percent");
    if (!cJSON_IsNumber(p) || gadget_sound_set_volume(p->valueint) != 0) {
        return gadget_command_error("percent must be an integer from 0 to 100");
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "percent", gadget_sound_volume());
    return gadget_command_ok_payload(r);
}

static cJSON *cmd_speaker_stop(const cJSON *params)
{
    (void)params;
    gadget_sound_stop();
    return gadget_command_ok();
}

static cJSON *cmd_speaker_status(const cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", state_name(gadget_sound_state()));
    if (gadget_sound_error()[0] != '\0') {
        cJSON_AddStringToObject(r, "error", gadget_sound_error());
    }
    cJSON_AddNumberToObject(r, "volume_percent", gadget_sound_volume());
    cJSON_AddBoolToObject(r, "speaker_on", true);   /* no mute setting on these boards */
    if (gadget_play_url_last()[0] != '\0') {
        cJSON_AddStringToObject(r, "last_url", gadget_play_url_last());
    }
    return gadget_command_ok_payload(r);
}

static const gadget_command_t s_speaker_cmds[] = {
    { "speaker.say",
      "Say something out loud on the gadget's speaker. Returns at once; the speech starts a few seconds "
      "later. Check speaker.status to know when it finished.",
      "{\"text\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":1000,\"description\":\"What to say, 1 to 1000 characters.\"}}",
      NULL, cmd_say },
    { "speaker.play_url",
      "Play a sound file from the internet on the gadget's speaker: a public https MP3 (or 16-bit WAV) of at "
      "most 1.5 MB, about a minute. Returns at once; speaker.status tells how it went.",
      "{\"url\":{\"type\":\"string\",\"minLength\":9,\"maxLength\":1024,\"description\":\"https:// link to the MP3.\"}}", NULL, cmd_play_url },
    { "speaker.beep", "Play a short beep.", NULL, NULL, cmd_beep },
    { "speaker.set_volume", "Set the speaker volume. It is kept across restarts.",
      "{\"percent\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100,\"description\":\"0 to 100.\"}}", NULL, cmd_set_volume },
    { "speaker.stop", "Stop whatever the speaker is playing.", NULL, NULL, cmd_speaker_stop },
    { "speaker.status", "What the speaker is doing (idle, fetching, playing, failed) and its volume.",
      NULL, NULL, cmd_speaker_status },
};

#define SPEAKER_COUNT (s_speaker ? (int)(sizeof(s_speaker_cmds) / sizeof(s_speaker_cmds[0])) : 0)

/* ---- display.show_animation (CAPABILITIES.md: status-light boards) ---- */

static cJSON *cmd_show_animation(const cJSON *params)
{
    (void)params;
    gadget_status_show_again();
    return gadget_command_ok();
}

static const gadget_command_t s_light_cmds[] = {
    { "display.show_animation",
      "Give the gadget's lights back to its own status animation (after chassis.set_light, for example).",
      NULL, NULL, cmd_show_animation },
};

#define LIGHT_COUNT (gadget_board_get()->show_status != NULL ? 1 : 0)

/* ---- registration ---- */

static void describe(cJSON *commands, const gadget_command_t *c)
{
    cJSON *spec = cJSON_AddObjectToObject(commands, c->name);
    cJSON_AddStringToObject(spec, "description", c->description);
    cJSON *req = c->required ? cJSON_Parse(c->required) : NULL;
    cJSON *opt = c->optional ? cJSON_Parse(c->optional) : NULL;
    if ((c->required && req == NULL) || (c->optional && opt == NULL)) {
        LISA_LOGE(TAG, "bad parameter JSON in %s", c->name);
    }
    cJSON_AddItemToObject(spec, "required", req ? req : cJSON_CreateObject());
    cJSON_AddItemToObject(spec, "optional", opt ? opt : cJSON_CreateObject());
}

/* Every command into link.register's commands_v2, the standard ones first. A name already
 * there (the link's own device.health and device.set_name) keeps the link's description. */
void gadget_describe_commands(cJSON *commands)
{
    const gadget_board_t *b = gadget_board_get();
    const struct {
        const gadget_command_t *list;
        int count;
    } groups[] = {
        { s_standard, STANDARD_COUNT },
        { s_speaker_cmds, SPEAKER_COUNT },
        { s_light_cmds, LIGHT_COUNT },
        { b->commands, b->command_count },
    };
    for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
        for (int i = 0; i < groups[g].count; i++) {
            if (cJSON_GetObjectItem(commands, groups[g].list[i].name) == NULL) {
                describe(commands, &groups[g].list[i]);
            }
        }
    }
}

/*
 * link.register's "capabilities" (CAPABILITIES.md). The board's events and
 * instructions are not sent: since contracts/gadget-identity.md the platform
 * has them, in the manifest of the product the gadget declares
 * (gadget_board_t.product_id).
 */
void gadget_fill_capabilities(cJSON *params)
{
    const gadget_board_t *b = gadget_board_get();
    cJSON *cap = cJSON_AddObjectToObject(params, "capabilities");
    /* Reported once the SDK's audio path exists; the board already says what it has. */
    cJSON_AddBoolToObject(cap, "speaker", s_speaker);
    cJSON_AddBoolToObject(cap, "microphone", s_voice);
    cJSON_AddBoolToObject(cap, "push_to_talk", s_voice);
    cJSON_AddBoolToObject(cap, "battery", b->read_power != NULL);
    cJSON_AddBoolToObject(cap, "buttons", b->poll_buttons != NULL);
    cJSON_AddBoolToObject(cap, "camera", false);
    cJSON *features = cJSON_AddArrayToObject(cap, "features");
    if (b->features != NULL) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s", b->features);
        for (char *save = NULL, *t = strtok_r(buf, ",", &save); t != NULL; t = strtok_r(NULL, ",", &save)) {
            cJSON_AddItemToArray(features, cJSON_CreateString(t));
        }
    }
}

/* ---- dispatch ---- */

cJSON *gadget_run_command(const char *name, const cJSON *params)
{
    const gadget_board_t *b = gadget_board_get();
    const gadget_command_t *c = NULL;
    for (int i = 0; i < STANDARD_COUNT && c == NULL; i++) {
        if (strcmp(s_standard[i].name, name) == 0) {
            c = &s_standard[i];
        }
    }
    for (int i = 0; i < SPEAKER_COUNT && c == NULL; i++) {
        if (strcmp(s_speaker_cmds[i].name, name) == 0) {
            c = &s_speaker_cmds[i];
        }
    }
    for (int i = 0; i < LIGHT_COUNT && c == NULL; i++) {
        if (strcmp(s_light_cmds[i].name, name) == 0) {
            c = &s_light_cmds[i];
        }
    }
    for (int i = 0; i < b->command_count && c == NULL; i++) {
        if (strcmp(b->commands[i].name, name) == 0) {
            c = &b->commands[i];
        }
    }
    if (c == NULL) {
        return gadget_command_error("unknown command");
    }
    cJSON *empty = NULL;
    if (!cJSON_IsObject(params)) {
        params = empty = cJSON_CreateObject();
    }
    /* Checked against the schema the agent was given, so a wrong call says what is expected. */
    char why[400];
    if (!gadget_params_check(c->required, c->optional, params, why, sizeof(why))) {
        cJSON_Delete(empty);
        LISA_LOGW(TAG, "%s refused: %s", name, why);
        return gadget_command_error(why);
    }
    uint64_t t0 = lisa_os_get_tick_ms();
    cJSON *r = c->run(params);
    cJSON_Delete(empty);
    if (r == NULL) {
        r = gadget_command_error("command returned nothing");
    }
    LISA_LOGI(TAG, "%s -> %s in %u ms", name, cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")) ? "ok" : "error",
              (unsigned)(lisa_os_get_tick_ms() - t0));
    return r;
}

int gadget_send_event(const char *name, cJSON *data)
{
    return impo_link_send_event(name, data);
}

int gadget_start(void)
{
    const gadget_board_t *b = gadget_board_get();
    LISA_LOGI(TAG, "board %s (%s), %d board commands, %d events", b->name, b->model_id, b->command_count,
              b->event_count);
    int r = b->init != NULL ? b->init() : 0;
    if (b->audio.speaker) {
        s_speaker = gadget_sound_start(b->audio.set_amp) == 0;
    }
    /* Voice needs the speaker for replies, the mic, and a button to start a turn. */
    if (s_speaker && b->audio.microphone && b->poll_buttons != NULL && gadget_voice_start() == 0) {
        s_voice = true;
        gadget_input_on_aux(gadget_voice_trigger);
    }
    gadget_status_start(s_voice);
    return r;
}
