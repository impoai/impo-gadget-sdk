/*
 * SDK status and power task: every 100 ms decides the state the status light
 * shows (gadget_policy) and hands changes to the board's show_status; every
 * second reads the battery for device.health and applies the battery policy
 * (warn once when low, power off when empty).
 */
#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "platform_arcs.h"
#include "impo_link.h"
#include "gadget_board.h"
#include "gadget_policy.h"
#include "gadget_sound.h"
#include "gadget_speech.h"
#include "gadget_status.h"
#include "gadget_voice.h"

#define TAG "gadget_status"
#include "lisa_log.h"

#define TICK_MS 100

static gadget_power_t s_power;
static bool s_power_ok;
static volatile gadget_status_t s_shown = (gadget_status_t)-1;
static gadget_power_policy_t s_policy;
static bool s_voice;

bool gadget_status_power(gadget_power_t *out)
{
    *out = s_power;
    return s_power_ok;
}

gadget_status_t gadget_status_current(void)
{
    return s_shown;
}

static gadget_net_t net_now(void)
{
    impo_link_state_t l = impo_link_state();
    if (l == IMPO_LINK_UNPAIRED) {
        return GADGET_NET_UNPAIRED;
    }
    if (!plat_wifi_has_ip()) {
        return GADGET_NET_NO_WIFI;
    }
    return l == IMPO_LINK_ONLINE ? GADGET_NET_ONLINE : GADGET_NET_CONNECTING;
}

static gadget_talk_t talk_now(void)
{
    if (!s_voice) {
        return GADGET_TALK_IDLE;
    }
    switch (gadget_voice_state()) {
    case GADGET_VOICE_LISTENING: return GADGET_TALK_LISTENING;
    case GADGET_VOICE_THINKING: return GADGET_TALK_THINKING;
    case GADGET_VOICE_SPEAKING: return GADGET_TALK_SPEAKING;
    default: return GADGET_TALK_IDLE;
    }
}

static void battery_tick(const gadget_board_t *b)
{
    gadget_power_t pw;
    s_power_ok = b->read_power(&pw) == 0;
    if (!s_power_ok) {
        return;
    }
    s_power = pw;
    switch (gadget_power_policy_step(&s_policy, &pw)) {
    case GADGET_POWER_WARN:
        LISA_LOGW(TAG, "battery low: %d%% (%d mV)", pw.battery_pct, pw.battery_mv);
        if (gadget_speech_say("My battery is getting low. Please charge me soon.") != 0) {
            gadget_sound_beep_down();
        }
        break;
    case GADGET_POWER_SHUT_DOWN:
        LISA_LOGE(TAG, "battery empty: %d%% (%d mV), powering off", pw.battery_pct, pw.battery_mv);
        gadget_sound_beep_down();
        vTaskDelay(pdMS_TO_TICKS(500));
        if (b->power_off != NULL) {
            b->power_off();
        }
        break;
    default:
        break;
    }
}

static void status_task(void *arg)
{
    const gadget_board_t *b = arg;
    int wd = plat_watchdog_register("gadget_stat", 10000);
    uint32_t n = 0;
    for (;;) {
        plat_watchdog_checkin(wd);
        if (b->read_power != NULL && n % (1000 / TICK_MS) == 0) {
            battery_tick(b);
        }
        if (n % (10000 / TICK_MS) == 0) {
            plat_wifi_watch_router();
        }
        n++;
        if (b->show_status != NULL) {
            gadget_sound_state_t ss = gadget_sound_state();
            gadget_status_in_t in = {
                .uptime_ms = (uint32_t)plat_uptime_ms(),
                .net = net_now(),
                .talk = talk_now(),
                .sound_playing = ss == GADGET_SOUND_PLAYING,
                .low_battery = s_policy.low,
                .charging = s_power_ok && s_power.charging,
            };
            gadget_status_t st = gadget_status_pick(&in);
            if (st != s_shown) {
                static const char *const names[] = { "booting", "unpaired", "connecting", "idle", "listening",
                                                     "thinking", "speaking", "offline", "low_battery", "charging",
                                                     "updating" };
                LISA_LOGI(TAG, "status: %s", (unsigned)st < sizeof(names) / sizeof(names[0]) ? names[st] : "?");
                s_shown = st;
                b->show_status(st);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void gadget_status_show_again(void)
{
    const gadget_board_t *b = gadget_board_get();
    if (b->show_animation != NULL) {
        b->show_animation();
    }
    if (b->show_status != NULL && (int)s_shown >= 0) {
        b->show_status(s_shown);
    }
}

int gadget_status_start(bool voice)
{
    const gadget_board_t *b = gadget_board_get();
    s_voice = voice;
    gadget_power_policy_init(&s_policy);
    if (b->read_power == NULL && b->show_status == NULL) {
        return 0;
    }
    return xTaskCreate(status_task, "gadget_stat", 3072 / sizeof(StackType_t), (void *)b, 2, NULL) == pdPASS ? 0 : -1;
}
