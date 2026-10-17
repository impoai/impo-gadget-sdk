/*
 * The robot dog's commands (CAPABILITIES.md chassis.* and imu.read, plus dog.pose
 * and dog.trick) and events. Commands return at once: a worker task wakes the
 * legs if needed (stand, up to 5 s) and runs the motion.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "gadget_board.h"
#include "platform_arcs.h"
#include "dog_board.h"
#include "dog_motion.h"
#include "dog_logic.h"
#include "platform_arcs.h"

#define TAG "dog_cmd"
#include "lisa_log.h"

#define WALK_CYCLE_MS 640
#define RUN_CYCLE_MS  320
#define TURN_CYCLE_MS 600
#define MOVE_DEFAULT_MS 3000
#define MOVE_MAX_MS     10000
#define DRIVE_DEFAULT_MS 500
#define WAKE_TIMEOUT_MS 5000
#define BUSY_WAIT_MS 15000         /* longer than the longest trick (dance, ~11 s) */
#define WORKER_WATCHDOG_MS 30000   /* a wake plus a submit; actions themselves run on the gait task */
#define MOTION_MAX_MS   20000      /* guardrail: no motion runs longer (longest real one: dance, ~11 s) */
#define MOVING_DPS      30.0f
#define MOVING_G        0.15f

typedef struct {
    service_v2_gait_action_t action;
    uint32_t cycles;
    uint8_t speed;
} job_t;

static QueueHandle_t s_jobs;

/* ---- worker ---- */

static void worker(void *arg)
{
    (void)arg;
    int wd = plat_watchdog_register("dog_cmd", WORKER_WATCHDOG_MS);
    uint64_t busy_since = 0;
    for (;;) {
        plat_watchdog_checkin(wd);
        /* Guardrail: whatever started it and whatever its parameters, a motion that keeps going
         * this long is a bug (it once spun for minutes on a cycle count of 0): stop it. */
        dog_motion_status_t ms;
        if (dog_motion_get_status(&ms) == 0 && ms.busy) {
            if (busy_since == 0) {
                busy_since = plat_uptime_ms();
            } else if (plat_uptime_ms() - busy_since > MOTION_MAX_MS) {
                LISA_LOGE(TAG, "%s still running after %u ms: stopping it", dog_motion_action_name(ms.current),
                          (unsigned)MOTION_MAX_MS);
                dog_motion_stop();
                busy_since = 0;
            }
        } else {
            busy_since = 0;
        }
        job_t j;
        if (xQueueReceive(s_jobs, &j, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        busy_since = 0;   /* a new command starts its own clock */
        /* Stop and power-off go at once: they preempt the motion instead of waiting for it. */
        if (j.action == SERVICE_V2_GAIT_ACTION_STOP || j.action == SERVICE_V2_GAIT_ACTION_ALL_OFF) {
            int r = j.action == SERVICE_V2_GAIT_ACTION_STOP ? dog_motion_stop() : dog_motion_all_off();
            LISA_LOGI(TAG, "%s: %d", dog_motion_action_name(j.action), r);
            continue;
        }
        /* Standing up first would roll it over; the wiggle is done on its back. */
        bool needs_wake = j.action != SERVICE_V2_GAIT_ACTION_ALL_OFF && j.action != SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE;
        if (!dog_motion_is_awake() && needs_wake) {
            int w = dog_motion_wake(false, WAKE_TIMEOUT_MS);
            LISA_LOGI(TAG, "wake: %d", w);
            if (w != 0) {
                continue;
            }
            if (j.action == SERVICE_V2_GAIT_ACTION_STAND) {
                continue;   /* waking already stands */
            }
        }
        /* Big motions start only from an idle gait (it may still be standing up, or finishing the
         * last trick): wait for it, unless a newer command arrives first. */
        int r = -EBUSY;
        uint32_t waited = 0;
        for (;;) {
            dog_motion_status_t st;
            bool busy = dog_motion_get_status(&st) == 0 && st.busy;
            if (!busy && (r = dog_motion_submit(j.action, j.cycles, j.speed)) != -EBUSY) {
                break;
            }
            if (waited >= BUSY_WAIT_MS || uxQueueMessagesWaiting(s_jobs) > 0) {
                r = -EBUSY;
                break;
            }
            plat_watchdog_checkin(wd);
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
        }
        LISA_LOGI(TAG, "%s x%u: %d (waited %u ms)", dog_motion_action_name(j.action), (unsigned)j.cycles, r,
                  (unsigned)waited);
    }
}

static cJSON *start(service_v2_gait_action_t action, uint32_t cycles, uint8_t speed, uint32_t estimate_ms)
{
    job_t j = { action, cycles, speed };
    /* A newer command replaces anything still waiting. */
    xQueueReset(s_jobs);
    if (xQueueSend(s_jobs, &j, 0) != pdTRUE) {
        return gadget_command_error("busy");
    }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "base", "responding");
    cJSON_AddBoolToObject(p, "moving", action != SERVICE_V2_GAIT_ACTION_STOP && action != SERVICE_V2_GAIT_ACTION_ALL_OFF);
    cJSON_AddStringToObject(p, "status", "started");
    cJSON_AddStringToObject(p, "action", dog_motion_action_name(action));
    if (estimate_ms > 0) {
        cJSON_AddNumberToObject(p, "estimated_ms", estimate_ms);
    }
    if (!dog_motion_is_awake() && action != SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE &&
        action != SERVICE_V2_GAIT_ACTION_ALL_OFF) {
        cJSON_AddStringToObject(p, "note", "standing up first");
    }
    return gadget_command_ok_payload(p);
}

static const char *str_param(const cJSON *params, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(params, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int int_param(const cJSON *params, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItem(params, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

/* ---- chassis.* ---- */

static cJSON *chassis_move(const cJSON *params)
{
    const char *dir = str_param(params, "direction");
    int ms = int_param(params, "duration_ms", MOVE_DEFAULT_MS);
    const cJSON *fast = cJSON_GetObjectItem(params, "fast");
    bool run = cJSON_IsTrue(fast);
    if (dir == NULL) {
        return gadget_command_error("direction is required: forward, back, left, right or stop");
    }
    if (ms < 1 || ms > MOVE_MAX_MS) {
        return gadget_command_error("duration_ms must be 1 to 10000");
    }
    service_v2_gait_action_t a;
    int cycle_ms;
    if (strcmp(dir, "stop") == 0) {
        return start(SERVICE_V2_GAIT_ACTION_STOP, 0, 50, 500);
    } else if (strcmp(dir, "forward") == 0) {
        a = run ? SERVICE_V2_GAIT_ACTION_RUN_FORWARD : SERVICE_V2_GAIT_ACTION_FORWARD;
        cycle_ms = run ? RUN_CYCLE_MS : WALK_CYCLE_MS;
    } else if (strcmp(dir, "back") == 0) {
        a = run ? SERVICE_V2_GAIT_ACTION_RUN_BACKWARD : SERVICE_V2_GAIT_ACTION_BACKWARD;
        cycle_ms = run ? RUN_CYCLE_MS : WALK_CYCLE_MS;
    } else if (strcmp(dir, "left") == 0) {
        a = SERVICE_V2_GAIT_ACTION_TURN_LEFT;
        cycle_ms = TURN_CYCLE_MS;
    } else if (strcmp(dir, "right") == 0) {
        a = SERVICE_V2_GAIT_ACTION_TURN_RIGHT;
        cycle_ms = TURN_CYCLE_MS;
    } else {
        return gadget_command_error("direction must be forward, back, left, right or stop");
    }
    uint32_t cycles = (uint32_t)((ms + cycle_ms - 1) / cycle_ms);
    return start(a, cycles, 50, cycles * (uint32_t)cycle_ms);
}

static float num_param(const cJSON *params, const char *key, float def)
{
    const cJSON *v = cJSON_GetObjectItem(params, key);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

/* chassis.drive: forward/turn in -1..1 pick a gait (dog_drive_pick) and its speed. */
static cJSON *chassis_drive(const cJSON *params)
{
    int ms = int_param(params, "duration_ms", DRIVE_DEFAULT_MS);
    if (ms < 1 || ms > MOVE_MAX_MS) {
        return gadget_command_error("duration_ms must be 1 to 10000");
    }
    dog_drive_t d = dog_drive_pick(num_param(params, "forward", 0), num_param(params, "turn", 0));
    service_v2_gait_action_t a;
    int cycle_ms;
    switch (d.dir) {
    case DOG_DRIVE_FORWARD:
        a = d.run ? SERVICE_V2_GAIT_ACTION_RUN_FORWARD : SERVICE_V2_GAIT_ACTION_FORWARD;
        cycle_ms = d.run ? RUN_CYCLE_MS : WALK_CYCLE_MS;
        break;
    case DOG_DRIVE_BACK:
        a = d.run ? SERVICE_V2_GAIT_ACTION_RUN_BACKWARD : SERVICE_V2_GAIT_ACTION_BACKWARD;
        cycle_ms = d.run ? RUN_CYCLE_MS : WALK_CYCLE_MS;
        break;
    case DOG_DRIVE_LEFT:
        a = SERVICE_V2_GAIT_ACTION_TURN_LEFT;
        cycle_ms = TURN_CYCLE_MS;
        break;
    case DOG_DRIVE_RIGHT:
        a = SERVICE_V2_GAIT_ACTION_TURN_RIGHT;
        cycle_ms = TURN_CYCLE_MS;
        break;
    default:
        return start(SERVICE_V2_GAIT_ACTION_STOP, 0, 50, 500);
    }
    uint32_t cycles = (uint32_t)((ms + cycle_ms - 1) / cycle_ms);
    return start(a, cycles, d.speed_percent, cycles * (uint32_t)cycle_ms);
}

static cJSON *chassis_stop(const cJSON *params)
{
    (void)params;
    return start(SERVICE_V2_GAIT_ACTION_STOP, 0, 50, 500);
}

static cJSON *chassis_dance(const cJSON *params)
{
    (void)params;
    return start(SERVICE_V2_GAIT_ACTION_DANCE, 0, 50, 11250);
}

static cJSON *chassis_status(const cJSON *params)
{
    (void)params;
    dog_motion_status_t st;
    dog_motion_get_status(&st);
    dog_imu_reading_t imu = {0};
    dog_motion_imu_read(&imu);
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "base", st.ready ? "responding" : "silent");
    cJSON_AddBoolToObject(p, "moving", st.busy);
    cJSON_AddStringToObject(p, "action", dog_motion_action_name(st.current));
    cJSON_AddBoolToObject(p, "legs_powered", st.awake);
    cJSON_AddStringToObject(p, "orientation",
                            imu.ready ? dog_orientation(imu.accel_g[0], imu.accel_g[1], imu.accel_g[2]) : "unknown");
    if (st.last_error != 0) {
        cJSON_AddNumberToObject(p, "last_error", st.last_error);
    }
    return gadget_command_ok_payload(p);
}

static int hex_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s == NULL) {
        return -1;
    }
    if (s[0] == '#') {
        s++;
    }
    unsigned v;
    if (strlen(s) != 6 || sscanf(s, "%6x", &v) != 1) {
        return -1;
    }
    *r = (uint8_t)(v >> 16);
    *g = (uint8_t)(v >> 8);
    *b = (uint8_t)v;
    return 0;
}

static cJSON *chassis_set_light(const cJSON *params)
{
    const char *effect = str_param(params, "effect");
    uint8_t r = 255, g = 255, b = 255;
    const char *color = str_param(params, "color");
    if (color != NULL && hex_color(color, &r, &g, &b) != 0) {
        return gadget_command_error("color must be #RRGGBB");
    }
    int bri = int_param(params, "brightness", 100);
    if (bri < 0 || bri > 100) {
        return gadget_command_error("brightness must be 0 to 100");
    }
    if (effect == NULL || dog_light_set(effect, r, g, b, bri) != 0) {
        return gadget_command_error("effect must be on, blink, breathe_slow, breathe_fast, flowing, show, off or auto");
    }
    dog_motion_status_t st;
    dog_motion_get_status(&st);
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "base", "responding");
    cJSON_AddBoolToObject(p, "moving", st.busy);
    return gadget_command_ok_payload(p);
}

/* ---- imu.read ---- */

static cJSON *imu_read(const cJSON *params)
{
    (void)params;
    dog_imu_reading_t m;
    if (dog_motion_imu_read(&m) != 0 || !m.ready) {
        return gadget_command_error("motion sensor not ready");
    }
    float rot = sqrtf(m.gyro_dps[0] * m.gyro_dps[0] + m.gyro_dps[1] * m.gyro_dps[1] + m.gyro_dps[2] * m.gyro_dps[2]);
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "orientation", dog_orientation(m.accel_g[0], m.accel_g[1], m.accel_g[2]));
    float acc = sqrtf(m.accel_g[0] * m.accel_g[0] + m.accel_g[1] * m.accel_g[1] + m.accel_g[2] * m.accel_g[2]);
    cJSON_AddBoolToObject(p, "moving", rot > MOVING_DPS || fabsf(acc - 1.0f) > MOVING_G);
    cJSON *a = cJSON_AddObjectToObject(p, "accel_g");
    cJSON_AddNumberToObject(a, "x", roundf(m.accel_g[0] * 100) / 100);
    cJSON_AddNumberToObject(a, "y", roundf(m.accel_g[1] * 100) / 100);
    cJSON_AddNumberToObject(a, "z", roundf(m.accel_g[2] * 100) / 100);
    cJSON *gy = cJSON_AddObjectToObject(p, "gyro_dps");
    cJSON_AddNumberToObject(gy, "x", roundf(m.gyro_dps[0] * 10) / 10);
    cJSON_AddNumberToObject(gy, "y", roundf(m.gyro_dps[1] * 10) / 10);
    cJSON_AddNumberToObject(gy, "z", roundf(m.gyro_dps[2] * 10) / 10);
    return gadget_command_ok_payload(p);
}

/* ---- dog.pose, dog.trick ---- */

typedef struct {
    const char *name;
    service_v2_gait_action_t action;
    uint32_t cycles;
    uint32_t estimate_ms;
} named_t;

static const named_t s_poses[] = {
    { "stand", SERVICE_V2_GAIT_ACTION_STAND, 0, 500 },
    { "sit", SERVICE_V2_GAIT_ACTION_SIT, 0, 600 },
    { "lie_down", SERVICE_V2_GAIT_ACTION_PRONE, 0, 800 },
    { "sleep", SERVICE_V2_GAIT_ACTION_STANDBY_FOLD, 0, 2500 },
};

static const named_t s_tricks[] = {
    { "handshake", SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_HANDSHAKE, 0, 4200 },
    { "left_handshake", SERVICE_V2_GAIT_ACTION_LEFT_FRONT_HANDSHAKE, 0, 4200 },
    { "greeting", SERVICE_V2_GAIT_ACTION_GREETING, 0, 3000 },
    { "scratch", SERVICE_V2_GAIT_ACTION_SCRATCH, 0, 3000 },
    { "kiss", SERVICE_V2_GAIT_ACTION_KISS, 0, 3000 },
    { "jump", SERVICE_V2_GAIT_ACTION_JUMP, 0, 1500 },
    { "wiggle", SERVICE_V2_GAIT_ACTION_WIGGLE_BUTT, 0, 3000 },
    { "pushup", SERVICE_V2_GAIT_ACTION_PUSHUP, 4, 5000 },
    { "stretch", SERVICE_V2_GAIT_ACTION_IDLE_STRETCH, 0, 4000 },
    { "kick", SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_KICK, 0, 3000 },
    { "spin", SERVICE_V2_GAIT_ACTION_SPIN, 13, 8000 },   /* 13 turn cycles of 600 ms */
    { "dance", SERVICE_V2_GAIT_ACTION_DANCE, 0, 11250 },
    { "handstand", SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP, 1, 4000 },
    { "front_flip", SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP, 2, 6000 },
    { "backflip", SERVICE_V2_GAIT_ACTION_BACKFLIP, 1, 3000 },
    { "tennis_serve", SERVICE_V2_GAIT_ACTION_TENNIS_SERVE, 0, 3000 },
    { "tennis_forehand", SERVICE_V2_GAIT_ACTION_TENNIS_FOREHAND, 0, 3000 },
    { "tennis_backhand", SERVICE_V2_GAIT_ACTION_TENNIS_BACKHAND, 0, 3000 },
    { "tennis_double_backhand", SERVICE_V2_GAIT_ACTION_TENNIS_DOUBLE_BACKHAND, 0, 3000 },
    { "paw", SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH, 0, 4000 },
    { "upside_wiggle", SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE, 0, 2500 },
};

static cJSON *run_named(const named_t *t, int n, const char *key, const cJSON *params)
{
    const char *want = str_param(params, key);
    for (int i = 0; want != NULL && i < n; i++) {
        if (strcmp(t[i].name, want) == 0) {
            if (t[i].action == SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE) {
                dog_imu_reading_t imu = {0};
                if (dog_motion_imu_read(&imu) != 0 || imu.pose != SERVICE_V2_IMU_POSE_UPSIDE_DOWN) {
                    return gadget_command_error("upside_wiggle needs the dog on its back");
                }
            }
            return start(t[i].action, t[i].cycles, 50, t[i].estimate_ms);
        }
    }
    return gadget_command_error("unknown name; see the command description for the list");
}

static cJSON *dog_pose(const cJSON *params)
{
    return run_named(s_poses, sizeof(s_poses) / sizeof(s_poses[0]), "pose", params);
}

static cJSON *dog_trick(const cJSON *params)
{
    return run_named(s_tricks, sizeof(s_tricks) / sizeof(s_tricks[0]), "trick", params);
}

static cJSON *dog_relax(const cJSON *params)
{
    (void)params;
    return start(SERVICE_V2_GAIT_ACTION_ALL_OFF, 0, 50, 0);
}

/* ---- tables ---- */

const gadget_command_t dog_commands[] = {
    { "chassis.drive",
      "Drive the robot dog for a while with a forward speed and a turn, each -1 to 1 (turn with forward 0 "
      "spins in place; above 0.7 it runs). The larger of the two decides; it stops and stands when the time "
      "is up. Returns at once.",
      NULL,
      "{\"forward\":{\"type\":\"number\",\"minimum\":-1,\"maximum\":1,\"description\":\"-1 (full speed back) to 1 (full speed forward); 0 if left out.\"},"
      "\"turn\":{\"type\":\"number\",\"minimum\":-1,\"maximum\":1,\"description\":\"-1 (left) to 1 (right); 0 if left out.\"},"
      "\"duration_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":10000,\"description\":\"How long, up to 10000; 500 if left out.\"}}",
      chassis_drive },
    { "chassis.move",
      "Walk the robot dog in one direction for a while, then it stops and stands. It stands up first "
      "if its legs are resting. Returns at once; the walk takes about duration_ms.",
      "{\"direction\":{\"type\":\"string\",\"enum\":[\"forward\",\"back\",\"left\",\"right\",\"stop\"],\"description\":\"forward, back, left (turn in place), right (turn in place) or stop.\"}}",
      "{\"duration_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":10000,\"description\":\"How long, 1 to 10000; 3000 if left out. A walking step takes 640 ms, a 90 degree turn about 4800 ms.\"},"
      "\"fast\":{\"type\":\"boolean\",\"description\":\"Run instead of walk (forward or back only).\"}}",
      chassis_move },
    { "chassis.stop", "Stop the robot dog at once and have it stand still.", NULL, NULL, chassis_stop },
    { "chassis.dance", "Make the robot dog dance for about 11 seconds.", NULL, NULL, chassis_dance },
    { "chassis.status",
      "Whether the robot dog is moving, what it is doing, whether its legs are powered, and which way up it is.",
      NULL, NULL, chassis_status },
    { "chassis.set_light",
      "Set the robot dog's light strip (its eyes are part of it) to an effect in a colour. It stays until "
      "display.show_animation or effect auto hands the light back to the dog's status colours.",
      "{\"effect\":{\"type\":\"string\",\"enum\":[\"on\",\"blink\",\"breathe_slow\",\"breathe_fast\",\"flowing\",\"show\",\"off\",\"auto\"],\"description\":\"on, blink, breathe_slow, breathe_fast, flowing, show (rainbow), off, or auto.\"}}",
      "{\"color\":{\"type\":\"string\",\"minLength\":7,\"maxLength\":7,\"description\":\"#RRGGBB; white if left out.\"},"
      "\"brightness\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100,\"description\":\"0 to 100; 100 if left out.\"}}",
      chassis_set_light },
    { "imu.read",
      "Read the robot dog's motion sensor: which way up it is (upright, upside_down, face_up, face_down, "
      "on_left_side, on_right_side or tilted), whether "
      "it is being moved, and acceleration in g and rotation in degrees per second.",
      NULL, NULL, imu_read },
    { "dog.pose",
      "Put the robot dog in a resting pose. lie_down also relaxes its legs afterwards; sleep folds "
      "its legs under it, the way it rests before standby.",
      "{\"pose\":{\"type\":\"string\",\"enum\":[\"stand\",\"sit\",\"lie_down\",\"sleep\"],\"description\":\"stand, sit, lie_down or sleep.\"}}",
      NULL, dog_pose },
    { "dog.trick",
      "Have the robot dog do a trick. Returns at once; a trick takes 2 to 8 seconds. backflip only works "
      "on battery with the USB cable unplugged. paw: sits and paddles a front paw. upside_wiggle: "
      "only while it lies on its back (after the upside_down event), wiggles its legs in the air.",
      "{\"trick\":{\"type\":\"string\",\"enum\":[\"handshake\",\"left_handshake\",\"greeting\",\"scratch\",\"kiss\",\"jump\",\"wiggle\",\"pushup\",\"stretch\",\"kick\",\"spin\",\"dance\",\"handstand\",\"front_flip\",\"backflip\",\"paw\",\"upside_wiggle\",\"tennis_serve\",\"tennis_forehand\",\"tennis_backhand\",\"tennis_double_backhand\"],"
      "\"description\":\"handshake, left_handshake, greeting, scratch, kiss, jump, wiggle, "
      "pushup, stretch, kick, spin, dance, handstand, front_flip, backflip, paw, upside_wiggle, tennis_serve, "
      "tennis_forehand, tennis_backhand or tennis_double_backhand.\"}}",
      NULL, dog_trick },
    { "dog.relax",
      "Switch the robot dog's leg motors off so its legs go limp; it stays where it is. Use it to save "
      "battery or before someone picks it up.",
      NULL, NULL, dog_relax },
};
const int dog_command_count = sizeof(dog_commands) / sizeof(dog_commands[0]);

const gadget_event_t dog_events[] = {
    { "picked_up", "Someone lifted the robot dog and is holding it off its feet, most likely to play with it." },
    { "put_down", "The robot dog was set back down on its feet." },
    { "shaken", "The robot dog was shaken; strength is gentle or hard. A hard shake means someone wants its attention now." },
    { "upside_down", "The robot dog was turned onto its back, legs in the air." },
};
const int dog_event_count = sizeof(dog_events) / sizeof(dog_events[0]);

static void on_imu_event(service_v2_imu_event_t ev)
{
    if (ev == SERVICE_V2_IMU_EVENT_UPSIDE_DOWN) {
        dog_motion_status_t st;
        dog_motion_get_status(&st);
        if (!st.busy) {   /* not when its own flip lands it there */
            cJSON *d = cJSON_CreateObject();
            cJSON_AddStringToObject(d, "orientation", "upside_down");
            gadget_send_event("upside_down", d);
        }
    }
}

/* Motion events from the latest IMU sample, every DOG_WATCH_MS (dog_watch_step decides). */
static void watch_task(void *arg)
{
    (void)arg;
    dog_watch_t w;
    dog_watch_init(&w);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DOG_WATCH_MS));
        dog_imu_reading_t m;
        dog_motion_status_t st;
        if (dog_motion_imu_read(&m) != 0 || !m.ready || dog_motion_get_status(&st) != 0) {
            continue;
        }
        dog_event_t ev = dog_watch_step(&w, (uint32_t)plat_uptime_ms(), m.accel_g[0], m.accel_g[1], m.accel_g[2],
                                        st.busy);
        if (ev.kind == DOG_EV_SHAKEN) {
            cJSON *d = cJSON_CreateObject();
            cJSON_AddNumberToObject(d, "peak_g", ev.peak_g);
            cJSON_AddStringToObject(d, "strength", ev.strength);
            LISA_LOGI(TAG, "shaken (%d.%d g)", (int)ev.peak_g, (int)(ev.peak_g * 10) % 10);
            gadget_send_event("shaken", d);
        } else if (ev.kind == DOG_EV_PICKED_UP || ev.kind == DOG_EV_PUT_DOWN) {
            cJSON *d = cJSON_CreateObject();
            cJSON_AddStringToObject(d, "orientation", ev.orientation);
            LISA_LOGI(TAG, "%s (%s)", ev.kind == DOG_EV_PICKED_UP ? "picked up" : "put down", ev.orientation);
            gadget_send_event(ev.kind == DOG_EV_PICKED_UP ? "picked_up" : "put_down", d);
        }
    }
}

int dog_commands_init(void)
{
    s_jobs = xQueueCreate(1, sizeof(job_t));
    if (s_jobs == NULL) {
        return -1;
    }
    dog_motion_set_imu_listener(on_imu_event);
    if (xTaskCreate(watch_task, "dog_watch", 3072 / sizeof(StackType_t), NULL, 2, NULL) != pdPASS) {
        return -1;
    }
    return xTaskCreate(worker, "dog_cmd", 4096 / sizeof(StackType_t), NULL, 4, NULL) == pdPASS ? 0 : -1;
}
