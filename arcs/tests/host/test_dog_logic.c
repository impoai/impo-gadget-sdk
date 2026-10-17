/* Host tests for boards/arcs_mini_v2_1/gadget/dog_logic.c. */
#include <string.h>

#include "host_check.h"
#include "dog_logic.h"

static void test_orientation(void)
{
    CHECK(strcmp(dog_orientation(0.06f, -0.04f, -0.95f), "upright") == 0, "bench reading is upright");
    CHECK(strcmp(dog_orientation(0, 0, 1), "upside_down") == 0, "z up is upside_down");
    CHECK(strcmp(dog_orientation(1, 0, 0), "face_up") == 0, "nose up");
    CHECK(strcmp(dog_orientation(-1, 0, 0), "face_down") == 0, "nose down");
    CHECK(strcmp(dog_orientation(0, -1, 0), "on_left_side") == 0, "left side");
    CHECK(strcmp(dog_orientation(0, 1, 0), "on_right_side") == 0, "right side");
    CHECK(strcmp(dog_orientation(0.6f, 0, -0.6f), "tilted") == 0, "45 degrees is tilted");
}

/* Feeds n samples of one reading; returns the first event. */
static dog_event_t feed(dog_watch_t *w, uint32_t *t, int n, float ax, float ay, float az, bool self)
{
    dog_event_t first = { DOG_EV_NONE, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        *t += DOG_WATCH_MS;
        dog_event_t e = dog_watch_step(w, *t, ax, ay, az, self);
        if (first.kind == DOG_EV_NONE) first = e;
    }
    return first;
}

static void test_watch(void)
{
    dog_watch_t w;
    uint32_t t = 100000;
    dog_watch_init(&w);
    CHECK(feed(&w, &t, 50, 0, 0, -1, false).kind == DOG_EV_NONE, "sitting still: nothing");

    /* Shake: alternate +2 g / 0 g on z for a second. */
    dog_event_t e = { DOG_EV_NONE, 0, 0, 0 };
    for (int i = 0; i < 10 && e.kind == DOG_EV_NONE; i++) {
        e = feed(&w, &t, 1, 0, 0, (i % 2) ? -3.2f : -0.2f, false);
    }
    CHECK(e.kind == DOG_EV_SHAKEN && strcmp(e.strength, "hard") == 0 && e.peak_g == 2.2f, "a hard shake (peak 2.2 g)");
    e = feed(&w, &t, 10, 0, 0, -1, false);
    for (int i = 0; i < 10; i++) {
        dog_event_t x = feed(&w, &t, 1, 0, 0, (i % 2) ? -1.8f : -1.0f, false);
        if (x.kind != DOG_EV_NONE) e = x;
    }
    CHECK(e.kind == DOG_EV_NONE, "a second shake within 8 s is not reported");
    feed(&w, &t, 90, 0, 0, -1, false);
    e.kind = DOG_EV_NONE;
    for (int i = 0; i < 10; i++) {
        dog_event_t x = feed(&w, &t, 1, 0, 0, (i % 2) ? -1.8f : -1.0f, false);
        if (x.kind != DOG_EV_NONE) e = x;
    }
    CHECK(e.kind == DOG_EV_SHAKEN && strcmp(e.strength, "gentle") == 0, "a gentle shake later");

    /* One bump is not a shake. */
    feed(&w, &t, 100, 0, 0, -1, false);
    e = feed(&w, &t, 1, 0, 0, -3.0f, false);
    e = feed(&w, &t, 20, 0, 0, -1, false);
    CHECK(e.kind == DOG_EV_NONE, "a single bump is not a shake");

    /* Picked up: off upright for 1.5 s. */
    e = feed(&w, &t, 10, 0, -1, 0, false);
    CHECK(e.kind == DOG_EV_NONE, "1 s on its side is not yet a pick-up");
    e = feed(&w, &t, 10, 0, -1, 0, false);
    CHECK(e.kind == DOG_EV_PICKED_UP && strcmp(e.orientation, "on_left_side") == 0, "picked up, with its orientation");
    feed(&w, &t, 60, 0, -1, 0, false);
    e = feed(&w, &t, 20, 0, 0, -1, false);
    CHECK(e.kind == DOG_EV_PUT_DOWN && strcmp(e.orientation, "upright") == 0, "put down upright");

    /* Its own gait: no events while it moves, nor for 2 s after. */
    feed(&w, &t, 60, 0, 0, -1, false);
    e.kind = DOG_EV_NONE;
    for (int i = 0; i < 40; i++) {
        dog_event_t x = feed(&w, &t, 1, (i % 2) ? 1.0f : 0, 0, (i % 2) ? -2.5f : 0.3f, true);
        if (x.kind != DOG_EV_NONE) e = x;
    }
    CHECK(e.kind == DOG_EV_NONE, "a backflip of its own sends nothing");
    e = feed(&w, &t, 15, 0, 0, 1, false);
    CHECK(e.kind == DOG_EV_NONE, "nor right after it");
    e = feed(&w, &t, 40, 0, 0, 1, false);
    CHECK(e.kind == DOG_EV_NONE, "landing upside down from its own trick is the new normal");
}

static void test_drive(void)
{
    dog_drive_t d = dog_drive_pick(0, 0);
    CHECK(d.dir == DOG_DRIVE_STOP, "0,0 stops");
    d = dog_drive_pick(0.5f, 0.1f);
    CHECK(d.dir == DOG_DRIVE_FORWARD && !d.run && d.speed_percent == 65, "half forward walks");
    d = dog_drive_pick(1.0f, 0);
    CHECK(d.dir == DOG_DRIVE_FORWARD && d.run && d.speed_percent == 100, "full forward runs");
    d = dog_drive_pick(-0.9f, 0.2f);
    CHECK(d.dir == DOG_DRIVE_BACK && d.run, "fast back runs back");
    d = dog_drive_pick(0, -0.4f);
    CHECK(d.dir == DOG_DRIVE_LEFT && !d.run, "turn alone spins left");
    d = dog_drive_pick(0.2f, 0.8f);
    CHECK(d.dir == DOG_DRIVE_RIGHT, "a larger turn wins");
    d = dog_drive_pick(5, 0);
    CHECK(d.speed_percent == 100, "values are clamped");
}

static void test_light(void)
{
    dog_light_effect_t e;
    CHECK(dog_light_effect_from_name("breathe_fast", &e) && e == DOG_LIGHT_BREATHE_FAST, "effect names");
    CHECK(!dog_light_effect_from_name("disco", &e), "unknown effect refused");
    dog_rgb_t px[DOG_PIXELS];
    dog_rgb_t red = { 200, 0, 0 };
    dog_light_frame(DOG_LIGHT_ON, red, 50, 0, px);
    CHECK(px[0].r == 100 && px[4].r == 100 && px[2].g == 0, "on at half brightness");
    dog_light_frame(DOG_LIGHT_BLINK, red, 100, 250, px);
    bool lit = px[0].r == 200;
    dog_light_frame(DOG_LIGHT_BLINK, red, 100, 750, px);
    CHECK(lit && px[0].r == 0, "blink alternates every 500 ms");
    dog_light_frame(DOG_LIGHT_BREATHE_SLOW, red, 100, 0, px);
    int low = px[0].r;
    dog_light_frame(DOG_LIGHT_BREATHE_SLOW, red, 100, 2000, px);
    CHECK(low < 20 && px[0].r == 200, "breathing: dim at 0, full at half period");
    int lit_count = 0;
    dog_light_frame(DOG_LIGHT_FLOWING, red, 100, 360, px);
    for (int i = 0; i < DOG_PIXELS; i++) lit_count += px[i].r == 200;
    CHECK(lit_count == 1 && px[3].r == 200, "flowing: one bright head, at pixel 3 after 360 ms");
    dog_light_frame(DOG_LIGHT_SHOW, red, 100, 0, px);
    CHECK(!(px[0].r == px[2].r && px[0].g == px[2].g && px[0].b == px[2].b), "show: pixels differ in colour");
    dog_light_frame(DOG_LIGHT_OFF, red, 100, 0, px);
    CHECK(px[0].r == 0 && px[4].r == 0, "off is dark");
}

int main(void)
{
    test_orientation();
    test_watch();
    test_drive();
    test_light();
    DONE();
}
