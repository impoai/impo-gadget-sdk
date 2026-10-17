/* Host tests for gadget/gadget_policy.c: status light choice and battery policy. */
#include "host_check.h"
#include "gadget_policy.h"

static void test_status(void)
{
    gadget_status_in_t in = { 10000, GADGET_NET_ONLINE, GADGET_TALK_IDLE, false, false, false };
    CHECK(gadget_status_pick(&in) == GADGET_STATUS_IDLE, "online and idle");
    in.charging = true;
    CHECK(gadget_status_pick(&in) == GADGET_STATUS_CHARGING, "charging shows when idle");
    in.net = GADGET_NET_CONNECTING;
    CHECK(gadget_status_pick(&in) == GADGET_STATUS_CONNECTING, "connecting beats charging");
    in.net = GADGET_NET_NO_WIFI;
    CHECK(gadget_status_pick(&in) == GADGET_STATUS_OFFLINE, "no Wi-Fi is offline");
    in.net = GADGET_NET_UNPAIRED;
    CHECK(gadget_status_pick(&in) == GADGET_STATUS_UNPAIRED, "unpaired");
    in.uptime_ms = 1000;
    CHECK(gadget_status_pick(&in) == GADGET_STATUS_BOOTING, "booting for the first 3 s");
    in.low_battery = true;
    CHECK(gadget_status_pick(&in) == GADGET_STATUS_LOW_BATTERY, "low battery beats boot and network");
    in.sound_playing = true;
    CHECK(gadget_status_pick(&in) == GADGET_STATUS_SPEAKING, "a sound playing shows speaking");
    in.talk = GADGET_TALK_THINKING;
    CHECK(gadget_status_pick(&in) == GADGET_STATUS_THINKING, "thinking during a turn");
    in.talk = GADGET_TALK_LISTENING;
    CHECK(gadget_status_pick(&in) == GADGET_STATUS_LISTENING, "listening beats everything");
}

static gadget_power_action_t step(gadget_power_policy_t *p, int pct, int mv, bool usb)
{
    gadget_power_t pw = { true, mv, pct, false, usb };
    return gadget_power_policy_step(p, &pw);
}

static void test_power(void)
{
    gadget_power_policy_t p;
    gadget_power_policy_init(&p);
    CHECK(step(&p, 60, 3900, false) == GADGET_POWER_NOTHING && !p.low, "60% is fine");
    CHECK(step(&p, 15, 3600, false) == GADGET_POWER_WARN && p.low, "15% warns");
    CHECK(step(&p, 14, 3590, false) == GADGET_POWER_NOTHING && p.low, "only once");
    CHECK(step(&p, 18, 3620, false) == GADGET_POWER_NOTHING, "a small rise does not re-arm");
    CHECK(step(&p, 15, 3600, false) == GADGET_POWER_NOTHING, "so no second warning");
    step(&p, 25, 3700, false);
    CHECK(step(&p, 15, 3600, false) == GADGET_POWER_WARN, "after recharging past 20% it warns again");

    gadget_power_action_t a = GADGET_POWER_NOTHING;
    int n = 0;
    while (a != GADGET_POWER_SHUT_DOWN && n < 30) {
        a = step(&p, 2, 3350, false);
        n++;
    }
    CHECK(a == GADGET_POWER_SHUT_DOWN && n == GADGET_BATTERY_OFF_SAMPLES, "shuts down after 10 s at 2%");

    gadget_power_policy_init(&p);
    for (int i = 0; i < 5; i++) step(&p, 2, 3350, false);
    step(&p, 10, 3500, false);   /* a reading under load bounced back */
    a = GADGET_POWER_NOTHING;
    for (int i = 0; i < 9; i++) a = step(&p, 2, 3350, false);
    CHECK(a != GADGET_POWER_SHUT_DOWN, "a good reading restarts the count");

    gadget_power_policy_init(&p);
    a = GADGET_POWER_NOTHING;
    for (int i = 0; i < 20; i++) a = step(&p, 40, 3200, false);
    CHECK(a == GADGET_POWER_SHUT_DOWN, "under 3.3 V shuts down whatever the percentage says");

    gadget_power_policy_init(&p);
    a = GADGET_POWER_NOTHING;
    for (int i = 0; i < 20; i++) a = step(&p, 1, 3100, true);
    CHECK(a == GADGET_POWER_NOTHING && !p.low, "never on USB power");

    gadget_power_t none = { false, 0, 0, false, false };
    CHECK(gadget_power_policy_step(&p, &none) == GADGET_POWER_NOTHING, "nothing without a battery");
}

int main(void)
{
    test_status();
    test_power();
    DONE();
}
