/* SDK policies; see gadget_policy.h. Pure C, host-tested. */
#include <string.h>

#include "gadget_policy.h"

gadget_status_t gadget_status_pick(const gadget_status_in_t *in)
{
    /* What the person is doing with it comes first, then what needs their attention. */
    switch (in->talk) {
    case GADGET_TALK_LISTENING: return GADGET_STATUS_LISTENING;
    case GADGET_TALK_SPEAKING: return GADGET_STATUS_SPEAKING;
    case GADGET_TALK_THINKING: return GADGET_STATUS_THINKING;
    default: break;
    }
    if (in->sound_playing) {
        return GADGET_STATUS_SPEAKING;
    }
    if (in->low_battery) {
        return GADGET_STATUS_LOW_BATTERY;
    }
    if (in->uptime_ms < GADGET_BOOT_SHOW_MS) {
        return GADGET_STATUS_BOOTING;
    }
    switch (in->net) {
    case GADGET_NET_UNPAIRED: return GADGET_STATUS_UNPAIRED;
    case GADGET_NET_NO_WIFI: return GADGET_STATUS_OFFLINE;
    case GADGET_NET_CONNECTING: return GADGET_STATUS_CONNECTING;
    default: break;
    }
    return in->charging ? GADGET_STATUS_CHARGING : GADGET_STATUS_IDLE;
}

void gadget_power_policy_init(gadget_power_policy_t *p)
{
    memset(p, 0, sizeof(*p));
}

gadget_power_action_t gadget_power_policy_step(gadget_power_policy_t *p, const gadget_power_t *pw)
{
    bool external = pw->usb_power || pw->charging;
    if (!pw->has_battery || external) {
        p->low_samples = 0;
        p->low = false;
        if (pw->has_battery && pw->battery_pct >= GADGET_BATTERY_REARM_PCT) {
            p->warned = false;
        }
        return GADGET_POWER_NOTHING;
    }
    if (pw->battery_pct >= GADGET_BATTERY_REARM_PCT) {
        p->warned = false;
    }
    p->low = pw->battery_pct <= GADGET_BATTERY_WARN_PCT;

    bool empty = pw->battery_pct <= GADGET_BATTERY_OFF_PCT || pw->battery_mv < GADGET_BATTERY_OFF_MV;
    p->low_samples = empty ? p->low_samples + 1 : 0;
    if (p->low_samples >= GADGET_BATTERY_OFF_SAMPLES) {
        return GADGET_POWER_SHUT_DOWN;
    }
    if (p->low && !p->warned) {
        p->warned = true;
        return GADGET_POWER_WARN;
    }
    return GADGET_POWER_NOTHING;
}
