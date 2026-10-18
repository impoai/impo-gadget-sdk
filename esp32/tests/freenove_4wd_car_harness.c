// SPDX-License-Identifier: MIT
// Runs the Freenove 4WD car's commands (main/freenove_4wd_car.c, the
// host-tested part) against fake motors and a fake clock.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static int64_t fake_now;
static int fake_duty[4];
static int fake_motor_writes;
static int fake_motor_failures;    // writes still to fail
static int fake_battery_mv = -1;
static int fake_battery_reads;

static bool car_hw_motors(const int duty[4]) {
    fake_motor_writes++;
    if (fake_motor_failures > 0) {
        fake_motor_failures--;
        return false;
    }
    memcpy(fake_duty, duty, sizeof(fake_duty));
    return true;
}
static int fake_servo_us[2];
static bool car_hw_servo(int channel, int pulse_us) {
    if (fake_motor_failures > 0) {
        fake_motor_failures--;
        return false;
    }
    fake_servo_us[channel] = pulse_us;
    return true;
}
static int car_hw_battery_mv(void) {
    fake_battery_reads++;
    return fake_battery_mv;
}
static int fake_distance_mm = -1;
static int car_hw_distance_mm(void) { return fake_distance_mm; }
static int64_t car_now_ms(void) { return fake_now; }
static void car_lock(void) {}
static void car_unlock(void) {}

#include "car_commands.inc"

static int failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

// Runs `command` with `json` parameters (NULL for none).
static cJSON *run(const char *command, const char *json) {
    cJSON *params = json ? cJSON_Parse(json) : NULL;
    if (json && !params) {
        fprintf(stderr, "bad test JSON: %s\n", json);
        exit(2);
    }
    cJSON *result = freenove_car_command(command, params);
    cJSON_Delete(params);
    return result;
}

static bool ok(cJSON *r) { return r && cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")); }

static const char *code(cJSON *r) {
    return cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "error"), "code"));
}

static const char *message(cJSON *r) {
    return cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "error"), "message"));
}

static cJSON *payload(cJSON *r) { return cJSON_GetObjectItem(r, "payload"); }

static bool refused(const char *command, const char *json, const char *needle) {
    cJSON *r = run(command, json);
    bool ok_ = r && !ok(r) && !strcmp(code(r), "invalid_param") && strstr(message(r), needle);
    if (!ok_) {
        char *text = r ? cJSON_PrintUnformatted(r) : NULL;
        fprintf(stderr, "%s %s: expected invalid_param with \"%s\", got %s\n", command,
                json ? json : "", needle, text ? text : "NULL");
        free(text);
    }
    cJSON_Delete(r);
    return ok_;
}

static void test_mixing(void) {
    int d[4];
    car_mix(1, 0, d);
    CHECK(d[0] == 4095 && d[1] == 4095 && d[2] == 4095 && d[3] == 4095);
    car_mix(-1, 0, d);
    CHECK(d[0] == -4095 && d[2] == -4095);
    car_mix(0.3, 0, d);
    CHECK(d[0] == 1600 + (int)lround(0.3 * 2495) && d[2] == d[0]);
    car_mix(0, 1, d);   // spin right: left wheels forward, right wheels back
    CHECK(d[0] == 4095 && d[1] == 4095 && d[2] == -4095 && d[3] == -4095);
    car_mix(0, -0.5, d);
    CHECK(d[0] < 0 && d[2] > 0 && d[0] == -d[2]);
    car_mix(1, 1, d);   // scaled down together: right side stops
    CHECK(d[0] == 4095 && d[2] == 0);
    car_mix(0.004, 0, d);
    CHECK(d[0] == 0 && d[2] == 0);
}

static void test_drive_ends_on_time(void) {
    fake_now = 1000;
    cJSON *r = run("chassis.drive", "{\"forward\":0.5,\"duration_ms\":300}");
    CHECK(ok(r));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(payload(r), "moving")));
    CHECK(cJSON_GetObjectItem(payload(r), "remaining_ms")->valueint == 300);
    CHECK(fake_duty[0] > 0 && fake_duty[3] > 0);
    cJSON_Delete(r);
    fake_now = 1299;
    car_tick();
    CHECK(s_car.moving && fake_duty[0] > 0);
    fake_now = 1300;
    car_tick();
    CHECK(!s_car.moving && !strcmp(s_car.stopped, "time_up"));
    CHECK(fake_duty[0] == 0 && fake_duty[1] == 0 && fake_duty[2] == 0 && fake_duty[3] == 0);

    r = run("chassis.drive", "{\"turn\":0.2}");   // 500 ms if left out
    CHECK(ok(r) && s_car.until_ms == 1800);
    cJSON_Delete(r);
    fake_now = 1800;
    car_tick();
    CHECK(!s_car.moving);

    r = run("chassis.move", "{\"direction\":\"back\",\"duration_ms\":10000}");
    CHECK(ok(r) && fake_duty[0] < 0 && fake_duty[2] < 0 && s_car.until_ms == 11800);
    CHECK(fake_duty[0] == -car_duty(0.5));   // speed 0.5 if left out
    cJSON_Delete(r);
    fake_now = 11800;
    car_tick();
    CHECK(!s_car.moving && fake_duty[0] == 0);
}

static void test_stop_preempts(void) {
    fake_now = 20000;
    cJSON *r = run("chassis.move", "{\"direction\":\"left\",\"speed\":1,\"duration_ms\":5000}");
    CHECK(ok(r) && fake_duty[0] == -4095 && fake_duty[2] == 4095);
    cJSON_Delete(r);
    int writes = fake_motor_writes;
    r = run("chassis.stop", NULL);
    CHECK(ok(r) && fake_motor_writes == writes + 1);
    CHECK(fake_duty[0] == 0 && fake_duty[2] == 0 && !s_car.moving);
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(payload(r), "stopped")), "stop"));
    cJSON_Delete(r);

    r = run("chassis.move", "{\"direction\":\"forward\"}");
    cJSON_Delete(r);
    r = run("chassis.move", "{\"direction\":\"stop\"}");
    CHECK(ok(r) && !s_car.moving && fake_duty[0] == 0);
    cJSON_Delete(r);

    r = run("chassis.drive", "{\"forward\":0.8,\"duration_ms\":4000}");
    cJSON_Delete(r);
    freenove_car_stop("link_lost");
    CHECK(!s_car.moving && fake_duty[0] == 0 && !strcmp(s_car.stopped, "link_lost"));
    freenove_car_stop("link_lost");   // not moving: the reason stays
    r = run("chassis.drive", "{\"forward\":0,\"turn\":0}");   // nothing to do is a stop
    CHECK(ok(r) && !s_car.moving && !strcmp(s_car.stopped, "stop"));
    cJSON_Delete(r);
}

static void test_motor_driver_missing(void) {
    fake_now = 30000;
    fake_motor_failures = 1;
    cJSON *r = run("chassis.drive", "{\"forward\":1}");
    CHECK(!ok(r) && !strcmp(code(r), "base_error"));
    CHECK(!s_car.moving && !s_car.base_ok && fake_duty[0] == 0);   // the stop that followed got through
    cJSON_Delete(r);

    fake_motor_failures = 3;   // the stop fails too (twice)
    r = run("chassis.drive", "{\"forward\":1}");
    CHECK(!ok(r) && !s_car.moving);
    cJSON_Delete(r);
    r = run("chassis.status", NULL);
    CHECK(ok(r) && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(payload(r), "base")), "silent"));
    cJSON_Delete(r);
    fake_motor_failures = 0;

    s_car.ticking = false;   // no task to end it: no drive at all
    r = run("chassis.drive", "{\"forward\":1}");
    CHECK(!ok(r) && !strcmp(code(r), "unavailable") && fake_duty[0] == 0);
    cJSON_Delete(r);
    s_car.ticking = true;
}

static void test_validation(void) {
    // The platform checks calls against the manifest; these checks keep the car
    // safe whatever arrives.
    CHECK(refused("chassis.drive", "{\"forward\":1.5}", "forward and turn must be -1 to 1"));
    CHECK(refused("chassis.drive", "{\"turn\":-2}", "turn"));
    CHECK(refused("chassis.drive", "{\"forward\":\"fast\"}", "forward"));
    CHECK(refused("chassis.drive", "{\"duration_ms\":0}", "1 to 10000"));
    CHECK(refused("chassis.drive", "{\"duration_ms\":10001}", "1 to 10000"));
    CHECK(refused("chassis.drive", "{\"duration_ms\":250.5}", "whole number"));
    CHECK(refused("chassis.move", NULL, "direction must be"));
    CHECK(refused("chassis.move", "{\"direction\":\"up\"}", "direction must be"));
    CHECK(refused("chassis.move", "{\"direction\":\"forward\",\"speed\":0}", "0.1 to 1"));
    CHECK(refused("chassis.move", "{\"direction\":\"forward\",\"duration_ms\":20000}", "1 to 10000"));
    cJSON *list = cJSON_CreateArray();
    cJSON *r = freenove_car_command("chassis.drive", list);
    CHECK(!ok(r) && strstr(message(r), "must be an object"));
    cJSON_Delete(r);
    cJSON_Delete(list);
    CHECK(run("chassis.dance", NULL) == NULL);   // not the car's: Link answers it
    // Parts not wired yet have no commands.
    CHECK(run("chassis.set_light", "{\"effect\":\"on\"}") == NULL);
    CHECK(refused("servo.set_angle", "{\"tilt\":40}", "tilt"));
    CHECK(refused("servo.set_angle", "{\"pan\":181}", "pan"));
    CHECK(refused("servo.set_angle", "{\"pan\":\"left\"}", "pan"));
    CHECK(run("speaker.beep", NULL) == NULL);
    CHECK(run("device.health", NULL) == NULL);
    CHECK(!s_car.moving);                        // no refused call moved it
}

static void test_battery(void) {
    int pct = -1, mv = -1;
    fake_battery_mv = -1;
    CHECK(!freenove_car_battery(&pct, &mv));
    cJSON *r = run("chassis.status", NULL);
    CHECK(cJSON_IsNull(cJSON_GetObjectItem(payload(r), "battery_v")));
    cJSON_Delete(r);
    fake_battery_mv = 948;   // on USB with no cells: reported, not as a battery
    CHECK(!freenove_car_battery(&pct, &mv));
    r = run("chassis.status", NULL);
    CHECK(fabs(cJSON_GetObjectItem(payload(r), "battery_v")->valuedouble - 0.95) < 1e-9);
    CHECK(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(payload(r), "warning")), "No charged battery"));
    cJSON_Delete(r);
    fake_battery_mv = 8400;
    CHECK(freenove_car_battery(&pct, &mv) && pct == 100 && mv == 8400);
    fake_battery_mv = 7200;
    CHECK(freenove_car_battery(&pct, &mv) && pct == 50);
    fake_battery_mv = 6100;
    r = run("chassis.status", NULL);
    CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(payload(r), "warning")), "The battery is nearly empty."));
    cJSON_Delete(r);
    fake_battery_mv = 5500;
    CHECK(freenove_car_battery(&pct, &mv) && pct == 0);
    // Every ask reads the divider again: the switch may have just been turned on.
    int reads = fake_battery_reads;
    fake_battery_mv = 948;
    CHECK(!freenove_car_battery(&pct, &mv));
    fake_battery_mv = 8000;
    r = run("chassis.status", NULL);
    CHECK(fabs(cJSON_GetObjectItem(payload(r), "battery_v")->valuedouble - 8.0) < 1e-9);
    cJSON_Delete(r);
    CHECK(freenove_car_battery(&pct, &mv) && mv == 8000);
    CHECK(fake_battery_reads == reads + 3);
}

static void test_sense(void) {
    fake_distance_mm = -1;
    cJSON *r = run("chassis.sense", NULL);
    CHECK(ok(r) && cJSON_IsNull(cJSON_GetObjectItem(payload(r), "distance_cm")));
    cJSON_Delete(r);
    fake_distance_mm = 253;
    r = run("chassis.sense", NULL);
    CHECK(ok(r) && fabs(cJSON_GetObjectItem(payload(r), "distance_cm")->valuedouble - 25.3) < 1e-9);
    cJSON_Delete(r);
}

static void test_servo(void) {
    cJSON *r = run("servo.set_angle", "{\"pan\":180}");
    CHECK(ok(r) && fake_servo_us[0] == 2500 && s_car.pan == 180 && s_car.tilt == 90);
    CHECK(cJSON_GetObjectItem(payload(r), "pan")->valueint == 180);
    cJSON_Delete(r);
    r = run("servo.set_angle", "{\"pan\":0,\"tilt\":135}");
    CHECK(ok(r) && fake_servo_us[0] == 500 && fake_servo_us[1] == 2000 && s_car.tilt == 135);
    cJSON_Delete(r);
    r = run("servo.set_angle", "{}");
    CHECK(!ok(r) && !strcmp(code(r), "invalid_param"));
    cJSON_Delete(r);
    fake_motor_failures = 1;   // the PCA9685 stops answering: the angle is not taken
    r = run("servo.set_angle", "{\"pan\":90}");
    CHECK(!ok(r) && !strcmp(code(r), "base_error") && s_car.pan == 0);
    cJSON_Delete(r);
    fake_motor_failures = 0;
}

static void test_register(void) {
    cJSON *commands = cJSON_CreateObject();
    freenove_car_describe(commands);
    int n = 0;
    cJSON *c;
    cJSON_ArrayForEach(c, commands) {
        n++;
        CHECK(cJSON_IsObject(c) && cJSON_GetArraySize(c) == 0);   // the name only
    }
    CHECK(n == (int)(sizeof(s_commands) / sizeof(s_commands[0])));
    cJSON *params = cJSON_CreateObject();
    freenove_car_capabilities(params);
    cJSON_AddItemToObject(params, "commands_v2", commands);
    char *text = cJSON_PrintUnformatted(params);
    printf("register_bytes %zu\n", strlen(text));
    CHECK(strlen(text) < 5000);   // link.register has 8 KB in all
    free(text);
    cJSON_Delete(params);
}

int main(void) {
    s_car.ticking = true;
    test_mixing();
    test_drive_ends_on_time();
    test_stop_preempts();
    test_motor_driver_missing();
    test_validation();
    test_battery();
    test_sense();
    test_servo();
    test_register();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
