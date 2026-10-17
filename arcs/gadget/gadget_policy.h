/*
 * SDK policies with no RTOS or hardware dependency (host-tested,
 * tests/host/test_gadget_policy.c): which state the status light shows, and
 * what to do about the battery. gadget_status.c feeds them every tick.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gadget_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- status light ---- */

typedef enum {
    GADGET_NET_UNPAIRED,     /* no Impo credentials: advertising for the app */
    GADGET_NET_NO_WIFI,
    GADGET_NET_CONNECTING,   /* Wi-Fi up, link not online yet */
    GADGET_NET_ONLINE,
} gadget_net_t;

typedef enum {
    GADGET_TALK_IDLE,
    GADGET_TALK_LISTENING,
    GADGET_TALK_THINKING,
    GADGET_TALK_SPEAKING,
} gadget_talk_t;

typedef struct {
    uint32_t uptime_ms;
    gadget_net_t net;
    gadget_talk_t talk;
    bool sound_playing;      /* speaker.say, beep, play_url outside a voice turn */
    bool low_battery;        /* from the power policy */
    bool charging;
} gadget_status_in_t;

#define GADGET_BOOT_SHOW_MS 3000

gadget_status_t gadget_status_pick(const gadget_status_in_t *in);

/* ---- battery ---- */

#define GADGET_BATTERY_WARN_PCT      15
#define GADGET_BATTERY_REARM_PCT     20
#define GADGET_BATTERY_OFF_PCT       3
#define GADGET_BATTERY_OFF_MV        3300
#define GADGET_BATTERY_OFF_SAMPLES   10    /* consecutive 1 s readings before powering off */

typedef enum {
    GADGET_POWER_NOTHING,
    GADGET_POWER_WARN,        /* tell the person once */
    GADGET_POWER_SHUT_DOWN,
} gadget_power_action_t;

typedef struct {
    bool warned;
    int low_samples;
    bool low;                 /* show the low-battery state */
} gadget_power_policy_t;

void gadget_power_policy_init(gadget_power_policy_t *p);
/* One reading per second. Never acts on external power or without a battery. */
gadget_power_action_t gadget_power_policy_step(gadget_power_policy_t *p, const gadget_power_t *pw);

#ifdef __cplusplus
}
#endif
