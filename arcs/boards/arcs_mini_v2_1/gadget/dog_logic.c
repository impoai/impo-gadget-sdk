/* Robot dog board decisions; see dog_logic.h. Pure C, host-tested. */
#include <math.h>
#include <string.h>

#include "dog_logic.h"

/* ---- orientation ---- */

#define LEVEL_G 0.75f   /* one axis this close to gravity names the side */

const char *dog_orientation(float ax, float ay, float az)
{
    static const struct {
        int axis;
        float sign;
        const char *name;
    } sides[] = {
        { 2, -1, "upright" },
        { 2, +1, "upside_down" },
        { 0, +1, "face_up" },        /* nose up: sitting back on its tail */
        { 0, -1, "face_down" },
        { 1, -1, "on_left_side" },
        { 1, +1, "on_right_side" },
    };
    const float a[3] = { ax, ay, az };
    for (unsigned i = 0; i < sizeof(sides) / sizeof(sides[0]); i++) {
        if (a[sides[i].axis] * sides[i].sign >= LEVEL_G) {
            return sides[i].name;
        }
    }
    return "tilted";
}

/* ---- motion events ---- */

void dog_watch_init(dog_watch_t *w)
{
    memset(w, 0, sizeof(*w));
    w->upright = w->reported_upright = true;
}

dog_event_t dog_watch_step(dog_watch_t *w, uint32_t now, float ax, float ay, float az, bool self_moving)
{
    dog_event_t ev = { DOG_EV_NONE, 0, NULL, NULL };
    const char *o = dog_orientation(ax, ay, az);
    bool now_upright = strcmp(o, "upright") == 0;

    if (self_moving) {
        w->self_moved_at = now;
    }
    bool quiet_self = !self_moving && now - w->self_moved_at >= DOG_SELF_MOVE_MS;
    if (!quiet_self) {
        /* Its own gait: forget partial shakes, and take its pose as the new normal. */
        w->hits = w->window = 0;
        w->peak = 0;
        w->upright = w->reported_upright = now_upright;
        w->changed_at = now;
        return ev;
    }

    float a = sqrtf(ax * ax + ay * ay + az * az);
    float excess = fabsf(a - 1.0f);
    if (excess > DOG_SHAKE_G) {
        w->hits++;
        w->peak = excess > w->peak ? excess : w->peak;
    }
    if (++w->window >= 1000 / DOG_WATCH_MS) {
        if (w->hits >= DOG_SHAKE_SAMPLES && (!w->any_shake || now - w->last_shake > DOG_SHAKE_QUIET_MS)) {
            w->any_shake = true;
            w->last_shake = now;
            ev.kind = DOG_EV_SHAKEN;
            ev.peak_g = roundf(w->peak * 10) / 10;
            ev.strength = w->peak > 2.0f ? "hard" : "gentle";
        }
        w->hits = w->window = 0;
        w->peak = 0;
        if (ev.kind != DOG_EV_NONE) {
            return ev;
        }
    }

    if (now_upright != w->upright) {
        w->upright = now_upright;
        w->changed_at = now;
    }
    if (w->upright != w->reported_upright && now - w->changed_at >= DOG_MOVE_SETTLE_MS &&
        (!w->any_move || now - w->last_move > DOG_MOVE_QUIET_MS)) {
        w->reported_upright = w->upright;
        w->any_move = true;
        w->last_move = now;
        ev.kind = w->upright ? DOG_EV_PUT_DOWN : DOG_EV_PICKED_UP;
        ev.orientation = o;
    }
    return ev;
}

/* ---- chassis.drive ---- */

static float clamp1(float v)
{
    return v > 1 ? 1 : v < -1 ? -1 : v;
}

dog_drive_t dog_drive_pick(float forward, float turn)
{
    forward = clamp1(forward);
    turn = clamp1(turn);
    float f = fabsf(forward), t = fabsf(turn);
    dog_drive_t d = { DOG_DRIVE_STOP, false, 0 };
    float m = f >= t ? f : t;
    if (m < 0.05f) {
        return d;
    }
    if (f >= t) {
        d.dir = forward > 0 ? DOG_DRIVE_FORWARD : DOG_DRIVE_BACK;
        d.run = f > 0.7f;
    } else {
        d.dir = turn > 0 ? DOG_DRIVE_RIGHT : DOG_DRIVE_LEFT;
    }
    d.speed_percent = (uint8_t)(30 + 70 * m + 0.5f);
    return d;
}

/* ---- light ---- */

bool dog_light_effect_from_name(const char *name, dog_light_effect_t *out)
{
    static const struct {
        const char *name;
        dog_light_effect_t e;
    } map[] = {
        { "off", DOG_LIGHT_OFF }, { "on", DOG_LIGHT_ON }, { "blink", DOG_LIGHT_BLINK },
        { "breathe_slow", DOG_LIGHT_BREATHE_SLOW }, { "breathe_fast", DOG_LIGHT_BREATHE_FAST },
        { "flowing", DOG_LIGHT_FLOWING }, { "show", DOG_LIGHT_SHOW },
    };
    for (unsigned i = 0; name != NULL && i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(name, map[i].name) == 0) {
            *out = map[i].e;
            return true;
        }
    }
    return false;
}

static dog_rgb_t scale(dog_rgb_t c, int permille)
{
    dog_rgb_t o = { (uint8_t)(c.r * permille / 1000), (uint8_t)(c.g * permille / 1000), (uint8_t)(c.b * permille / 1000) };
    return o;
}

/* 0..1000 triangle-ish breath with a smooth curve. */
static int breath(uint32_t t, uint32_t period)
{
    float x = (float)(t % period) / (float)period;
    float v = 0.5f - 0.5f * cosf(2 * (float)M_PI * x);
    return (int)(50 + 950 * v);
}

static dog_rgb_t wheel(uint32_t pos)   /* 0..767 */
{
    pos %= 768;
    dog_rgb_t c;
    if (pos < 256) {
        c.r = (uint8_t)(255 - pos); c.g = (uint8_t)pos; c.b = 0;
    } else if (pos < 512) {
        pos -= 256;
        c.r = 0; c.g = (uint8_t)(255 - pos); c.b = (uint8_t)pos;
    } else {
        pos -= 512;
        c.r = (uint8_t)pos; c.g = 0; c.b = (uint8_t)(255 - pos);
    }
    return c;
}

void dog_light_frame(dog_light_effect_t e, dog_rgb_t color, int brightness, uint32_t t, dog_rgb_t out[DOG_PIXELS])
{
    if (brightness < 0) brightness = 0;
    if (brightness > 100) brightness = 100;
    int level = brightness * 10;   /* permille */
    memset(out, 0, sizeof(dog_rgb_t) * DOG_PIXELS);
    switch (e) {
    case DOG_LIGHT_OFF:
        break;
    case DOG_LIGHT_ON:
        for (int i = 0; i < DOG_PIXELS; i++) out[i] = scale(color, level);
        break;
    case DOG_LIGHT_BLINK:
        if ((t / 500) % 2 == 0) {
            for (int i = 0; i < DOG_PIXELS; i++) out[i] = scale(color, level);
        }
        break;
    case DOG_LIGHT_BREATHE_SLOW:
    case DOG_LIGHT_BREATHE_FAST: {
        int b = breath(t, e == DOG_LIGHT_BREATHE_SLOW ? 4000 : 1200);
        for (int i = 0; i < DOG_PIXELS; i++) out[i] = scale(color, level * b / 1000);
        break;
    }
    case DOG_LIGHT_FLOWING: {
        /* A head moving 0..4..0 with a dimmer neighbour. */
        int step = (int)((t / 120) % (2 * (DOG_PIXELS - 1)));
        int head = step < DOG_PIXELS ? step : 2 * (DOG_PIXELS - 1) - step;
        out[head] = scale(color, level);
        if (head > 0) out[head - 1] = scale(color, level / 6);
        if (head < DOG_PIXELS - 1) out[head + 1] = scale(color, level / 6);
        break;
    }
    case DOG_LIGHT_SHOW:
        for (int i = 0; i < DOG_PIXELS; i++) out[i] = scale(wheel(t / 8 + (uint32_t)i * 150), level);
        break;
    }
}
