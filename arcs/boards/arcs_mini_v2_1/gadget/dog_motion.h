/*
 * Robot dog motion and sensors, for the board file. Wraps the V2.1 drivers
 * ported from mtt1.0-impo (dog_actuators, dog_gait, dog_sensors, dog_imu_interaction).
 *
 * Servos boot unpowered and the gait player refuses every action except
 * ALL_OFF until dog_motion_wake() has run once (stand, or assembly pose).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dog_gait.h"
#include "dog_imu_interaction.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Actuators, sensors (touch, MPU6050), gait worker, IMU worker. Call once from board init. */
int dog_motion_init(void);

/* Stand up (or assembly pose) and authorise motion. Blocks until the pose is
 * queued or timeout_ms passes; call from a worker, not a callback. */
int dog_motion_wake(bool assembly, uint32_t timeout_ms);
bool dog_motion_is_awake(void);

/* Queue an action; pre-empts the current one. cycles 0 = the action's default
 * (0 for walk/run/turn means "until the next command"); speed 20..100 %, 0 = 50.
 * 0 queued, -EACCES not woken, -EINVAL bad cycles, -EBUSY, -ENOTSUP (music beat). */
int dog_motion_submit(service_v2_gait_action_t action, uint32_t cycles, uint8_t speed_percent);
/* Smooth return to standing. */
int dog_motion_stop(void);
/* Emergency: all servo PWM off at once, legs go limp. */
int dog_motion_all_off(void);

typedef struct {
    bool ready;
    bool awake;               /* motion authorised */
    bool busy;
    service_v2_gait_action_t current;
    uint32_t cycles_done;
    int last_error;
} dog_motion_status_t;
int dog_motion_get_status(dog_motion_status_t *out);

bool dog_motion_action_from_name(const char *name, service_v2_gait_action_t *action);
const char *dog_motion_action_name(service_v2_gait_action_t action);

/* Gait STARTED / FINISHED (see dog_gait.h). */
void dog_motion_set_listener(dog_gait_listener_t cb);

/* ---- sensors ---- */

typedef struct {
    bool ready;
    service_v2_imu_pose_t pose;   /* NORMAL = upright, UPSIDE_DOWN, OTHER = tilted */
    float accel_g[3];             /* x, y, z; +-2 g range */
    float gyro_dps[3];            /* +-250 dps range */
} dog_imu_reading_t;
/* Latest sample of the 20 ms IMU worker (no extra I2C traffic). */
int dog_motion_imu_read(dog_imu_reading_t *out);
/* "upright", "upside_down", "tilted" or "unknown" (CAPABILITIES.md orientation words). */
const char *dog_motion_orientation_name(service_v2_imu_pose_t pose);

/* Confirmed IMU events: upside down (300 ms stable), sustained shake ("dizzy"). */
void dog_motion_set_imu_listener(dog_imu_listener_t cb);

/* Touch pads: 0 = PA09, 1 = PA13. */
int dog_motion_touch_read(int pad, bool *touched);

/* ---- RGB strip (5 pixels, eyes are 2 and 4; brightness capped at 50 %) ---- */
int dog_motion_rgb_set(uint8_t r, uint8_t g, uint8_t b);                       /* system owner */
int dog_motion_rgb_strip(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness); /* manual owner, 0..100 */
int dog_motion_rgb_off(void);
int dog_motion_rgb_release(void);   /* hand the strip back to the system owner */

#ifdef __cplusplus
}
#endif
