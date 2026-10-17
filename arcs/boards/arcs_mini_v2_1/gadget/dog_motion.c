/* Robot dog motion and sensors wrapper; see dog_motion.h. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "shell.h"

#include "dog_actuators.h"
#include "dog_gait.h"
#include "dog_imu_interaction.h"
#include "dog_motion.h"
#include "dog_sensors.h"

#define TAG "dog_motion"
#include "lisa_log.h"

/* MPU6050 at +-2 g and +-250 dps (dog_sensors.h). */
#define ACCEL_LSB_PER_G   16384.0f
#define GYRO_LSB_PER_DPS  131.0f

static bool s_ready;
static volatile bool s_awake;

int dog_motion_init(void)
{
    int first = 0;
    int ret = service_v2_sensors_init();
    if (ret != 0) {
        LISA_LOGW(TAG, "sensors init: %d", ret);
        first = ret;
    }
    ret = service_v2_actuators_init();
    if (ret != 0) {
        LISA_LOGE(TAG, "actuators init: %d", ret);
        return ret;
    }
    ret = service_v2_gait_init();
    if (ret != 0) {
        LISA_LOGE(TAG, "gait init: %d", ret);
        return ret;
    }
    ret = service_v2_imu_interaction_init();
    if (ret != 0) {
        LISA_LOGW(TAG, "imu interaction init: %d", ret);
        if (first == 0) first = ret;
    }
    s_ready = true;
    LISA_LOGI(TAG, "motion ready (servos off until wake)");
    return first;
}

int dog_motion_wake(bool assembly, uint32_t timeout_ms)
{
    if (!s_ready) return -ENODEV;
    int ret = service_v2_gait_wake_pose(assembly ? SERVICE_V2_GAIT_ACTION_ASSEMBLY : SERVICE_V2_GAIT_ACTION_STAND,
                                        timeout_ms);
    if (ret == 0) s_awake = true;
    LISA_LOGI(TAG, "wake (%s): %d", assembly ? "assembly" : "stand", ret);
    return ret;
}

bool dog_motion_is_awake(void)
{
    return s_awake;
}

int dog_motion_submit(service_v2_gait_action_t action, uint32_t cycles, uint8_t speed_percent)
{
    if (!s_ready) return -ENODEV;
    return service_v2_gait_submit_speed(action, cycles, speed_percent);
}

int dog_motion_stop(void)
{
    return dog_motion_submit(SERVICE_V2_GAIT_ACTION_STOP, 0U, 0U);
}

int dog_motion_all_off(void)
{
    if (!s_ready) return -ENODEV;
    return service_v2_gait_submit(SERVICE_V2_GAIT_ACTION_ALL_OFF, 0U);
}

int dog_motion_get_status(dog_motion_status_t *out)
{
    memset(out, 0, sizeof(*out));
    service_v2_gait_status_t st;
    if (!s_ready || service_v2_gait_get_status(&st) != 0) return -ENODEV;
    out->ready = st.ready;
    out->awake = s_awake;
    out->busy = st.busy;
    out->current = st.current;
    out->cycles_done = st.cycles_done;
    out->last_error = st.last_error;
    return 0;
}

bool dog_motion_action_from_name(const char *name, service_v2_gait_action_t *action)
{
    return service_v2_gait_action_from_name(name, action);
}

const char *dog_motion_action_name(service_v2_gait_action_t action)
{
    return service_v2_gait_action_name(action);
}

void dog_motion_set_listener(dog_gait_listener_t cb)
{
    dog_gait_set_listener(cb);
}

int dog_motion_imu_read(dog_imu_reading_t *out)
{
    memset(out, 0, sizeof(*out));
    service_v2_imu_interaction_status_t st;
    if (service_v2_imu_interaction_get_status(&st) != 0) return -ENODEV;
    out->ready = st.ready;
    out->pose = st.pose;
    out->accel_g[0] = st.accel_x / ACCEL_LSB_PER_G;
    out->accel_g[1] = st.accel_y / ACCEL_LSB_PER_G;
    out->accel_g[2] = st.accel_z / ACCEL_LSB_PER_G;
    out->gyro_dps[0] = st.gyro_x / GYRO_LSB_PER_DPS;
    out->gyro_dps[1] = st.gyro_y / GYRO_LSB_PER_DPS;
    out->gyro_dps[2] = st.gyro_z / GYRO_LSB_PER_DPS;
    return st.ready ? 0 : -EAGAIN;
}

const char *dog_motion_orientation_name(service_v2_imu_pose_t pose)
{
    switch (pose) {
    case SERVICE_V2_IMU_POSE_NORMAL: return "upright";
    case SERVICE_V2_IMU_POSE_UPSIDE_DOWN: return "upside_down";
    case SERVICE_V2_IMU_POSE_OTHER: return "tilted";
    default: return "unknown";
    }
}

void dog_motion_set_imu_listener(dog_imu_listener_t cb)
{
    dog_imu_set_listener(cb);
}

int dog_motion_touch_read(int pad, bool *touched)
{
    if (pad < 0 || pad >= SERVICE_V2_TOUCH_CHANNEL_COUNT) return -EINVAL;
    return service_v2_touch_read_channel((service_v2_touch_channel_t)pad, touched);
}

int dog_motion_rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    return service_v2_actuators_rgb_set(r, g, b);
}

int dog_motion_rgb_strip(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
    return service_v2_actuators_rgb_strip_set(5U, r, g, b, brightness);
}

int dog_motion_rgb_off(void)
{
    return service_v2_actuators_rgb_off();
}

int dog_motion_rgb_release(void)
{
    return service_v2_actuators_rgb_release();
}

/* ---- bench shell: dog wake|assembly|status|imu|touch|rgb r g b [bri]|rgb off|off|<action> [cycles] [speed] ---- */

static int dog_cmd(int argc, char **argv)
{
    Shell *sh = shellGetCurrent();
    if (argc < 2) {
        shellPrint(sh, "usage: dog wake|assembly|status|imu|touch|off|rgb r g b [bri]|rgb off|<action> [cycles] [speed]\n");
        return -1;
    }
    const char *c = argv[1];
    if (strcmp(c, "wake") == 0 || strcmp(c, "assembly") == 0) {
        shellPrint(sh, "wake: %d\n", dog_motion_wake(strcmp(c, "assembly") == 0, 5000U));
    } else if (strcmp(c, "status") == 0) {
        dog_motion_status_t st;
        int r = dog_motion_get_status(&st);
        shellPrint(sh, "ret=%d ready=%d awake=%d busy=%d current=%s cycles_done=%u last_error=%d\n", r, st.ready,
                   st.awake, st.busy, dog_motion_action_name(st.current), (unsigned)st.cycles_done, st.last_error);
    } else if (strcmp(c, "imu") == 0) {
        dog_imu_reading_t m;
        int r = dog_motion_imu_read(&m);
        shellPrint(sh, "ret=%d orientation=%s accel_mg=%d,%d,%d gyro_cdps=%d,%d,%d\n", r,
                   dog_motion_orientation_name(m.pose), (int)(m.accel_g[0] * 1000), (int)(m.accel_g[1] * 1000),
                   (int)(m.accel_g[2] * 1000), (int)(m.gyro_dps[0] * 100), (int)(m.gyro_dps[1] * 100),
                   (int)(m.gyro_dps[2] * 100));
    } else if (strcmp(c, "touch") == 0) {
        bool a = false, b = false;
        int ra = dog_motion_touch_read(0, &a), rb = dog_motion_touch_read(1, &b);
        shellPrint(sh, "pa09=%d(%d) pa13=%d(%d)\n", a, ra, b, rb);
    } else if (strcmp(c, "off") == 0) {
        shellPrint(sh, "all-off: %d\n", dog_motion_all_off());
    } else if (strcmp(c, "rgb") == 0) {
        if (argc >= 3 && strcmp(argv[2], "off") == 0) {
            shellPrint(sh, "rgb off: %d\n", dog_motion_rgb_off());
        } else if (argc >= 5) {
            int bri = argc >= 6 ? atoi(argv[5]) : 100;
            shellPrint(sh, "rgb: %d\n", dog_motion_rgb_strip((uint8_t)atoi(argv[2]), (uint8_t)atoi(argv[3]),
                                                             (uint8_t)atoi(argv[4]), (uint8_t)bri));
        } else {
            shellPrint(sh, "usage: dog rgb r g b [bri] | dog rgb off\n");
        }
    } else {
        service_v2_gait_action_t a;
        if (!dog_motion_action_from_name(c, &a)) {
            shellPrint(sh, "unknown action %s\n", c);
            return -1;
        }
        uint32_t cycles = argc >= 3 ? (uint32_t)atoi(argv[2]) : 0U;
        uint8_t speed = argc >= 4 ? (uint8_t)atoi(argv[3]) : 0U;
        shellPrint(sh, "%s: %d\n", dog_motion_action_name(a), dog_motion_submit(a, cycles, speed));
    }
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 dog, dog_cmd, dog motion bench commands);
