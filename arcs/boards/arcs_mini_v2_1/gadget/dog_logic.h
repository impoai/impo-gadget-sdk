/*
 * The robot dog board's decisions, with no RTOS or driver dependency
 * (host-tested, tests/host/test_dog_logic.c): which way up it is, when a
 * motion is worth an event, how chassis.drive maps to gaits, and what its
 * five-pixel strip shows at a given moment.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- orientation (CAPABILITIES.md words) ---- */

/* From the accelerometer in g, board axes: upright reads z = -1, nose down x = -1, left side down y = -1
 * (measured upright on the bench: 0.06, -0.04, -0.95). */
const char *dog_orientation(float ax, float ay, float az);

/* ---- motion events: shaken, picked_up, put_down (as impo-gadget-sdk's sparkbot_imu.c) ---- */

#define DOG_WATCH_MS          100     /* sample interval */
#define DOG_SHAKE_G           0.6f    /* beyond gravity, on one sample */
#define DOG_SHAKE_SAMPLES     3       /* within a second: a shake, not a bump */
#define DOG_SHAKE_QUIET_MS    8000    /* one "shaken" per this long */
#define DOG_MOVE_SETTLE_MS    1500    /* off upright (or back) this long before it counts */
#define DOG_MOVE_QUIET_MS     5000    /* one pick-up or put-down per this long */
#define DOG_SELF_MOVE_MS      2000    /* its own gaits shake it: no events until this long after one */

typedef enum { DOG_EV_NONE, DOG_EV_SHAKEN, DOG_EV_PICKED_UP, DOG_EV_PUT_DOWN } dog_event_kind_t;

typedef struct {
    dog_event_kind_t kind;
    float peak_g;             /* shaken */
    const char *strength;     /* shaken: gentle or hard */
    const char *orientation;  /* picked_up, put_down */
} dog_event_t;

typedef struct {
    int hits, window;
    float peak;
    uint32_t last_shake, last_move, changed_at, self_moved_at;
    bool upright, reported_upright, any_shake, any_move;
} dog_watch_t;

void dog_watch_init(dog_watch_t *w);
/* One sample every DOG_WATCH_MS. self_moving: a gait is running. Returns at most one event. */
dog_event_t dog_watch_step(dog_watch_t *w, uint32_t now_ms, float ax, float ay, float az, bool self_moving);

/* ---- chassis.drive ---- */

typedef enum { DOG_DRIVE_STOP, DOG_DRIVE_FORWARD, DOG_DRIVE_BACK, DOG_DRIVE_LEFT, DOG_DRIVE_RIGHT } dog_drive_dir_t;

typedef struct {
    dog_drive_dir_t dir;
    bool run;                 /* the fast gait, for |forward| > 0.7 */
    uint8_t speed_percent;    /* 30..100 */
} dog_drive_t;

/* forward and turn in -1..1 (clamped); the larger one decides, turn alone spins in place. */
dog_drive_t dog_drive_pick(float forward, float turn);

/* ---- light strip ---- */

#define DOG_PIXELS 5

typedef enum {
    DOG_LIGHT_OFF,
    DOG_LIGHT_ON,
    DOG_LIGHT_BLINK,          /* 500 ms on, 500 ms off */
    DOG_LIGHT_BREATHE_SLOW,   /* 4 s */
    DOG_LIGHT_BREATHE_FAST,   /* 1.2 s */
    DOG_LIGHT_FLOWING,        /* a bright pixel runs along the strip */
    DOG_LIGHT_SHOW,           /* rainbow */
} dog_light_effect_t;

typedef struct {
    uint8_t r, g, b;
} dog_rgb_t;

/* chassis.set_light words; false when unknown. */
bool dog_light_effect_from_name(const char *name, dog_light_effect_t *out);
/* The strip at time t for an effect in a colour, brightness 0..100. */
void dog_light_frame(dog_light_effect_t e, dog_rgb_t color, int brightness, uint32_t t_ms, dog_rgb_t out[DOG_PIXELS]);

#ifdef __cplusplus
}
#endif
