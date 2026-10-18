// SPDX-License-Identifier: MIT
/*
 * Freenove 4WD Car Kit for ESP32 (FNK0053): an ESP32-WROVER (ESP32-D0WD-V3,
 * 4 MB flash) on Freenove's car board, with a CH340 USB bridge. Facts from
 * https://github.com/Freenove/Freenove_4WD_Car_Kit_for_ESP32 (Tutorial.pdf,
 * "Pins of the Car" and chapters 2 to 4, and the sketches below); the code is
 * this firmware's own.
 *
 * - I2C on GPIO13 (SDA) and GPIO14 (SCL), shared by:
 *   - a PCA9685 at 0x5F at 50 Hz (Freenove_4WD_Car_For_ESP32.h). Servo 1 on
 *     channel 0 turns the ultrasonic head: 90 is straight ahead, larger looks
 *     left (02.2_Ultrasonic_Ranging_Car, get_distance()). Servo 2 on channel 1
 *     tilts the camera head, which Freenove keeps at 80 to 180 (06.3,
 *     Servo_2_Angle()). The motors' two inputs: M1 15/14, M2 9/8, M3 12/13,
 *     M4 10/11. M1 and M2 are the left wheels, M3 and M4 the right, and a
 *     positive duty on the first input drives forward (01.1_Car_Move_and_Turn).
 *     Below a duty of 1600 of 4095 the gear motors stall (Motor_Move()).
 *   - a PCF8574 at 0x20 with the three line-tracking sensors on bits 0 to 2,
 *     left, middle, right; a 1 sees the line (04.1_Tracking_Sensor, Track_Car()).
 * - GPIO32 drives the 12 WS2812 lights (GRB, 01.6_WS2812) and is also the
 *   battery divider, a quarter of the voltage of the two 18650 cells (8.4 V
 *   full; Tutorial.pdf 2.4 and 01.4_Battery_level, batteryCoefficient 4). The
 *   battery is read between light updates, with the light driver released.
 * - GPIO33 reads the two photoresistors as one divider: about 2048 when they
 *   see the same light, lower when the left one sees more (03.2, Light_Car()
 *   turns left below its starting value).
 * - GPIO12 and GPIO15: the HC-SR04 ultrasonic trigger and echo, up to 3 m
 *   (02.1_Ultrasonic_Ranging, Get_Sonar()). Both are strapping pins, left low.
 * - GPIO2: a passive buzzer through a transistor, loudest near 2 kHz (01.3).
 * - GPIO0: the BOOT button, shared with the infrared receiver (05.1).
 * - The camera (OV2640 on the WROVER's camera connector) and the optional
 *   LED matrix (VK16K33 at 0x71) are not used.
 *
 * The wheels and servos run off the cells: on USB alone the commands work,
 * the PCA9685 answers, and nothing moves. A drive is always bounded (10 s at
 * most) and ends on its own, on chassis.stop, when the Link session drops,
 * and before a restart.
 */
#include "freenove_4wd_car.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "command_params.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "link.car";

// ---- Commands (host-tested) ----
// From here to "---- Hardware" builds on the host too
// (tests/freenove_4wd_car_harness.c stands in for the car_hw_* calls).

#define CAR_MAX_MS 10000
#define CAR_DEFAULT_MS 500
#define CAR_MOTOR_MIN 1600
#define CAR_MOTOR_MAX 4095
#define CAR_LEDS 12
#define CAR_DEFAULT_SPEED 0.5
#define CAR_DEFAULT_BRIGHTNESS 50
#define CAR_BATTERY_PRESENT_MV 5000   // under this there is no charged cell
#define CAR_BATTERY_EMPTY_MV 6000
#define CAR_BATTERY_FULL_MV 8400

// Hardware, defined under "---- Hardware" (or by the host harness).
static bool car_hw_motors(const int duty[4]);   // M1..M4, -4095..4095
static bool car_hw_servo(int channel, int pulse_us);
static bool car_hw_beep(int hz, int ms);
static void car_hw_beep_off(void);
static void car_hw_lights(const uint8_t px[CAR_LEDS][3]);
static int car_hw_distance_mm(void);            // -1 without an echo
static int car_hw_line(void);                   // bits 0..2 left, middle, right; -1 no answer
static int car_hw_light_level(void);            // 0..4095; -1 on failure
static int64_t car_now_ms(void);
static void car_lock(void);     // no-ops until freenove_car_init()
static void car_unlock(void);

typedef enum {
    FX_AUTO, FX_OFF, FX_ON, FX_BLINK, FX_BREATHE_SLOW, FX_BREATHE_FAST, FX_FLOWING, FX_SHOW,
} car_effect_t;

static const char *const CAR_EFFECTS[] = {
    "auto", "off", "on", "blink", "breathe_slow", "breathe_fast", "flowing", "show",
};

static struct {
    bool ticking;            // the task that ends drives runs: nothing moves without it
    bool base_ok;            // the PCA9685 took the last write
    bool moving;
    double forward, turn;
    int64_t until_ms;
    const char *stopped;     // why the last drive ended
    int pan, tilt;
    car_effect_t effect;
    uint8_t color[3];
    int brightness;
    int64_t effect_since_ms;
    uint8_t status[3];       // led_status.c's colour, shown under FX_AUTO
    int battery_mv;          // -1 until the first reading
} s_car = {
    .stopped = "not_started", .pan = 90, .tilt = 90, .effect = FX_AUTO,
    .color = {255, 255, 255}, .brightness = CAR_DEFAULT_BRIGHTNESS, .battery_mv = -1,
};

static cJSON *car_error(const char *code, const char *message) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_AddObjectToObject(result, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return result;
}

static cJSON *car_ok(cJSON *payload) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddItemToObject(result, "payload", payload ? payload : cJSON_CreateObject());
    return result;
}

static double car_number(const cJSON *params, const char *key, double fallback) {
    const cJSON *v = cJSON_GetObjectItem(params, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

// The signed duty for a wheel speed of -1..1: off, or at least the stall duty.
static int car_duty(double v) {
    double a = fabs(v);
    if (a < 0.01) return 0;
    int d = CAR_MOTOR_MIN + (int)lround(a * (CAR_MOTOR_MAX - CAR_MOTOR_MIN));
    if (d > CAR_MOTOR_MAX) d = CAR_MOTOR_MAX;
    return v < 0 ? -d : d;
}

// forward and turn (each -1..1, turn > 0 to the right) to M1..M4. Both sides
// are scaled down together when their sum would pass full speed.
static void car_mix(double forward, double turn, int duty[4]) {
    double left = forward + turn, right = forward - turn;
    double m = fmax(1.0, fmax(fabs(left), fabs(right)));
    duty[0] = duty[1] = car_duty(left / m);
    duty[2] = duty[3] = car_duty(right / m);
}

static int car_battery_percent(int mv) {
    if (mv <= CAR_BATTERY_EMPTY_MV) return 0;
    if (mv >= CAR_BATTERY_FULL_MV) return 100;
    return (mv - CAR_BATTERY_EMPTY_MV) * 100 / (CAR_BATTERY_FULL_MV - CAR_BATTERY_EMPTY_MV);
}

// With the lock held: what every chassis.* result reports.
static void car_add_state(cJSON *p) {
    cJSON_AddStringToObject(p, "base", s_car.base_ok ? "responding" : "silent");
    cJSON_AddBoolToObject(p, "moving", s_car.moving);
    if (s_car.moving) {
        cJSON_AddNumberToObject(p, "forward", s_car.forward);
        cJSON_AddNumberToObject(p, "turn", s_car.turn);
        int64_t left = s_car.until_ms - car_now_ms();
        cJSON_AddNumberToObject(p, "remaining_ms", left > 0 ? (double)left : 0);
    } else {
        cJSON_AddStringToObject(p, "stopped", s_car.stopped);
    }
    if (s_car.battery_mv < 0) {
        cJSON_AddNullToObject(p, "battery_v");
        return;
    }
    cJSON_AddNumberToObject(p, "battery_v", round(s_car.battery_mv / 10.0) / 100.0);
    if (s_car.battery_mv < CAR_BATTERY_PRESENT_MV) {
        cJSON_AddStringToObject(p, "warning",
                                "No charged battery (or the power switch is off): the wheels and "
                                "servos run off the two 18650 cells and will not move on USB alone.");
    } else if (s_car.battery_mv < CAR_BATTERY_EMPTY_MV + 400) {
        cJSON_AddStringToObject(p, "warning", "The battery is nearly empty.");
    }
}

// With the lock held. Tries twice: the wheels must not keep going.
static void car_stop_locked(const char *reason) {
    static const int zero[4] = {0, 0, 0, 0};
    s_car.base_ok = car_hw_motors(zero) || car_hw_motors(zero);
    s_car.stopped = reason;
    s_car.moving = false;
}

static cJSON *car_drive(double forward, double turn, int ms) {
    int duty[4];
    car_mix(forward, turn, duty);
    cJSON *payload = cJSON_CreateObject();
    car_lock();
    if (!duty[0] && !duty[2]) {
        car_stop_locked("stop");
    } else if (!s_car.ticking) {
        car_unlock();
        cJSON_Delete(payload);
        return car_error("unavailable", "the car's drive timer did not start, so it will not drive");
    } else if (!car_hw_motors(duty)) {
        car_stop_locked("base_error");
        s_car.base_ok = false;   // even if the stop got through
        car_unlock();
        cJSON_Delete(payload);
        return car_error("base_error",
                         "the motor driver (PCA9685 at I2C 0x5F) did not answer; the car is not moving");
    } else {
        s_car.base_ok = true;
        s_car.moving = true;
        s_car.forward = forward;
        s_car.turn = turn;
        s_car.until_ms = car_now_ms() + ms;
    }
    car_add_state(payload);
    car_unlock();
    return car_ok(payload);
}

static cJSON *cmd_drive(const cJSON *params) {
    return car_drive(car_number(params, "forward", 0), car_number(params, "turn", 0),
                     (int)car_number(params, "duration_ms", CAR_DEFAULT_MS));
}

static cJSON *cmd_move(const cJSON *params) {
    const char *direction = cJSON_GetStringValue(cJSON_GetObjectItem(params, "direction"));
    double speed = car_number(params, "speed", CAR_DEFAULT_SPEED);
    int ms = (int)car_number(params, "duration_ms", CAR_DEFAULT_MS);
    static const struct { const char *name; int forward, turn; } moves[] = {
        {"forward", 1, 0}, {"back", -1, 0}, {"left", 0, -1}, {"right", 0, 1}, {"stop", 0, 0},
    };
    for (size_t i = 0; i < sizeof(moves) / sizeof(moves[0]); i++) {
        if (!strcmp(direction, moves[i].name)) {
            return car_drive(moves[i].forward * speed, moves[i].turn * speed, ms);
        }
    }
    return car_error("invalid_param", "direction must be forward, back, left, right or stop");
}

static cJSON *cmd_stop(const cJSON *params) {
    (void)params;
    cJSON *payload = cJSON_CreateObject();
    car_lock();
    car_stop_locked("stop");
    car_add_state(payload);
    car_unlock();
    return car_ok(payload);
}

static cJSON *cmd_status(const cJSON *params) {
    (void)params;
    cJSON *payload = cJSON_CreateObject();
    car_lock();
    car_add_state(payload);
    cJSON_AddNumberToObject(payload, "pan", s_car.pan);
    cJSON_AddNumberToObject(payload, "tilt", s_car.tilt);
    cJSON_AddStringToObject(payload, "light", CAR_EFFECTS[s_car.effect]);
    car_unlock();
    return car_ok(payload);
}

static uint8_t car_scale(uint8_t c, double k) {
    double v = c * k;
    return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)lround(v);
}

// One frame of the lights at `now`, with the lock held.
static void car_light_frame(int64_t now, uint8_t px[CAR_LEDS][3]) {
    int64_t t = now - s_car.effect_since_ms;
    double k = s_car.brightness / 100.0;
    for (int i = 0; i < CAR_LEDS; i++) {
        double level = 0;
        const uint8_t *c = s_car.color;
        uint8_t wheel[3];
        switch (s_car.effect) {
        case FX_AUTO:
            c = s_car.status;
            level = 1;
            break;
        case FX_OFF:
            break;
        case FX_ON:
            level = k;
            break;
        case FX_BLINK:
            level = (t / 500) % 2 ? 0 : k;
            break;
        case FX_BREATHE_SLOW:
        case FX_BREATHE_FAST: {
            double period = s_car.effect == FX_BREATHE_SLOW ? 4000 : 1500;
            level = k * (0.5 - 0.5 * cos(2 * 3.14159265358979 * (double)(t % (int64_t)period) / period));
            break;
        }
        case FX_FLOWING: {
            int behind = (int)((t / 80 - i) % CAR_LEDS + CAR_LEDS) % CAR_LEDS;
            level = behind < 4 ? k * (4 - behind) / 4.0 : 0;
            break;
        }
        case FX_SHOW: {
            int h = (int)((i * 256 / CAR_LEDS + t / 10) % 256);
            int seg = h / 85, f = (h % 85) * 3;
            uint8_t up = (uint8_t)f, down = (uint8_t)(255 - f);
            wheel[0] = seg == 0 ? down : seg == 1 ? 0 : up;
            wheel[1] = seg == 0 ? up : seg == 1 ? down : 0;
            wheel[2] = seg == 0 ? 0 : seg == 1 ? up : down;
            c = wheel;
            level = k;
            break;
        }
        }
        for (int j = 0; j < 3; j++) px[i][j] = car_scale(c[j], level);
    }
}

static bool car_light_animated(car_effect_t effect) {
    return effect >= FX_BLINK;
}

static int car_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static cJSON *cmd_set_light(const cJSON *params) {
    const char *effect = cJSON_GetStringValue(cJSON_GetObjectItem(params, "effect"));
    const char *color = cJSON_GetStringValue(cJSON_GetObjectItem(params, "color"));
    int fx = -1;
    for (int i = 0; i < (int)(sizeof(CAR_EFFECTS) / sizeof(CAR_EFFECTS[0])); i++) {
        if (!strcmp(effect, CAR_EFFECTS[i])) fx = i;
    }
    if (fx < 0) {
        return car_error("invalid_param",
                         "effect must be on, blink, breathe_slow, breathe_fast, flowing, show, off or auto");
    }
    uint8_t rgb[3] = {255, 255, 255};
    if (color) {
        for (int i = 0; i < 3; i++) {
            int hi = color[0] == '#' ? car_hex(color[1 + 2 * i]) : -1;
            int lo = hi < 0 ? -1 : car_hex(color[2 + 2 * i]);
            if (lo < 0) {
                return car_error("invalid_param", "color must be #RRGGBB, for example #ff8000");
            }
            rgb[i] = (uint8_t)(hi * 16 + lo);
        }
    }
    uint8_t px[CAR_LEDS][3];
    cJSON *payload = cJSON_CreateObject();
    car_lock();
    s_car.effect = (car_effect_t)fx;
    memcpy(s_car.color, rgb, sizeof(rgb));
    s_car.brightness = (int)car_number(params, "brightness", CAR_DEFAULT_BRIGHTNESS);
    s_car.effect_since_ms = car_now_ms();
    car_light_frame(s_car.effect_since_ms, px);
    car_add_state(payload);
    cJSON_AddStringToObject(payload, "light", CAR_EFFECTS[fx]);
    car_unlock();
    car_hw_lights(px);
    return car_ok(payload);
}

static cJSON *cmd_beep(const cJSON *params) {
    int hz = (int)car_number(params, "frequency_hz", 2000);
    int ms = (int)car_number(params, "duration_ms", 200);
    if (!car_hw_beep(hz, ms)) return car_error("buzzer_error", "the buzzer could not be started");
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "frequency_hz", hz);
    cJSON_AddNumberToObject(payload, "duration_ms", ms);
    return car_ok(payload);
}

// 0..180 degrees to 500..2500 us, the range Freenove maps its servos over.
static int car_servo_us(int angle) {
    return 500 + angle * 2000 / 180;
}

static cJSON *cmd_servo(const cJSON *params) {
    const cJSON *pan = cJSON_GetObjectItem(params, "pan");
    const cJSON *tilt = cJSON_GetObjectItem(params, "tilt");
    if (!pan && !tilt) return car_error("invalid_param", "give pan, tilt or both");
    bool ok = true;
    cJSON *payload = cJSON_CreateObject();
    car_lock();
    if (pan && (ok = car_hw_servo(0, car_servo_us(pan->valueint)))) s_car.pan = pan->valueint;
    if (ok && tilt && (ok = car_hw_servo(1, car_servo_us(tilt->valueint)))) s_car.tilt = tilt->valueint;
    cJSON_AddNumberToObject(payload, "pan", s_car.pan);
    cJSON_AddNumberToObject(payload, "tilt", s_car.tilt);
    car_unlock();
    if (!ok) {
        cJSON_Delete(payload);
        return car_error("base_error", "the servo driver (PCA9685 at I2C 0x5F) did not answer");
    }
    return car_ok(payload);
}

static cJSON *cmd_sense(const cJSON *params) {
    (void)params;
    cJSON *payload = cJSON_CreateObject();
    int mm = car_hw_distance_mm();
    if (mm < 0) {
        cJSON_AddNullToObject(payload, "distance_cm");
    } else {
        cJSON_AddNumberToObject(payload, "distance_cm", mm / 10.0);
    }
    car_lock();
    cJSON_AddNumberToObject(payload, "pan", s_car.pan);
    car_unlock();
    int line = car_hw_line();
    if (line < 0) {
        cJSON_AddNullToObject(payload, "line");
    } else {
        cJSON *l = cJSON_AddObjectToObject(payload, "line");
        cJSON_AddBoolToObject(l, "left", line & 1);
        cJSON_AddBoolToObject(l, "middle", line & 2);
        cJSON_AddBoolToObject(l, "right", line & 4);
    }
    int light = car_hw_light_level();
    if (light < 0) {
        cJSON_AddNullToObject(payload, "light");
    } else {
        cJSON *l = cJSON_AddObjectToObject(payload, "light");
        cJSON_AddNumberToObject(l, "level", light);
        cJSON_AddStringToObject(l, "brighter", light < 1848 ? "left" : light > 2248 ? "right" : "even");
    }
    car_lock();
    bool unpowered = s_car.battery_mv >= 0 && s_car.battery_mv < CAR_BATTERY_PRESENT_MV;
    car_unlock();
    if (unpowered) {
        cJSON_AddStringToObject(payload, "warning",
                                "No charged battery (or the power switch is off): the sensors may "
                                "be unpowered, so these readings may be wrong.");
    }
    return car_ok(payload);
}

typedef struct {
    const char *name;
    const char *description;
    const char *required;   // JSON Schema properties, as in link.register
    const char *optional;
    cJSON *(*run)(const cJSON *params);
} car_command_t;

#define DURATION_MS "\"duration_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":10000," \
                    "\"description\":\"How long, 1 to 10000; 500 if left out.\"}"

static const car_command_t s_commands[] = {
    { "chassis.drive",
      "Drive the 4WD car for a while: forward speed and turn, each -1 to 1 (turn alone spins it in "
      "place). It stops by itself when the time is up. Returns at once.",
      NULL,
      "{\"forward\":{\"type\":\"number\",\"minimum\":-1,\"maximum\":1,\"description\":\"-1 (full speed back) to 1 (full speed forward); 0 if left out.\"},"
      "\"turn\":{\"type\":\"number\",\"minimum\":-1,\"maximum\":1,\"description\":\"-1 (left) to 1 (right); 0 if left out.\"},"
      DURATION_MS "}",
      cmd_drive },
    { "chassis.move",
      "Drive the 4WD car in one direction for a while (left and right spin it in place), or stop it. "
      "Returns at once.",
      "{\"direction\":{\"type\":\"string\",\"enum\":[\"forward\",\"back\",\"left\",\"right\",\"stop\"],\"description\":\"forward, back, left, right or stop.\"}}",
      "{" DURATION_MS ","
      "\"speed\":{\"type\":\"number\",\"minimum\":0.1,\"maximum\":1,\"description\":\"0.1 to 1; 0.5 if left out.\"}}",
      cmd_move },
    { "chassis.stop", "Stop the 4WD car's wheels at once.", NULL, NULL, cmd_stop },
    { "chassis.status",
      "Whether the car's motor driver answers, whether it is moving (and for how much longer), why "
      "it last stopped, the battery voltage, the servo angles and the light effect.",
      NULL, NULL, cmd_status },
    { "chassis.set_light",
      "Set the car's 12 lights to an effect in a colour. It stays until effect auto gives the lights "
      "back to the connection status colours.",
      "{\"effect\":{\"type\":\"string\",\"enum\":[\"on\",\"blink\",\"breathe_slow\",\"breathe_fast\",\"flowing\",\"show\",\"off\",\"auto\"],\"description\":\"on, blink, breathe_slow, breathe_fast, flowing, show (rainbow), off, or auto.\"}}",
      "{\"color\":{\"type\":\"string\",\"minLength\":7,\"maxLength\":7,\"description\":\"#RRGGBB; white if left out.\"},"
      "\"brightness\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100,\"description\":\"0 to 100; 50 if left out.\"}}",
      cmd_set_light },
    { "chassis.sense",
      "Read the car's sensors: the ultrasonic distance in cm (null with no echo within 3 m; it looks "
      "where the pan servo points), the three line sensors under it (true sees a dark line), and "
      "which side the light is brighter on.",
      NULL, NULL, cmd_sense },
    { "servo.set_angle",
      "Turn the car's servos: pan turns the ultrasonic head (90 straight ahead, larger to the left), "
      "tilt the camera head. Give one or both.",
      NULL,
      "{\"pan\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":180,\"description\":\"0 (right) to 180 (left); 90 ahead.\"},"
      "\"tilt\":{\"type\":\"integer\",\"minimum\":80,\"maximum\":180,\"description\":\"80 to 180; 90 at start.\"}}",
      cmd_servo },
    { "speaker.beep",
      "Sound the car's buzzer once. Returns at once.",
      NULL,
      "{\"frequency_hz\":{\"type\":\"integer\",\"minimum\":100,\"maximum\":8000,\"description\":\"Pitch, 100 to 8000; 2000 (loudest) if left out.\"},"
      "\"duration_ms\":{\"type\":\"integer\",\"minimum\":10,\"maximum\":3000,\"description\":\"10 to 3000; 200 if left out.\"}}",
      cmd_beep },
};

void freenove_car_describe(cJSON *commands) {
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        const car_command_t *c = &s_commands[i];
        cJSON *spec = cJSON_AddObjectToObject(commands, c->name);
        cJSON_AddStringToObject(spec, "description", c->description);
        cJSON *req = c->required ? cJSON_Parse(c->required) : NULL;
        cJSON *opt = c->optional ? cJSON_Parse(c->optional) : NULL;
        cJSON_AddItemToObject(spec, "required", req ? req : cJSON_CreateObject());
        cJSON_AddItemToObject(spec, "optional", opt ? opt : cJSON_CreateObject());
    }
}

void freenove_car_capabilities(cJSON *params) {
    cJSON *caps = cJSON_AddObjectToObject(params, "capabilities");
    cJSON_AddBoolToObject(caps, "battery", true);
    cJSON_AddBoolToObject(caps, "buttons", true);
    cJSON *features = cJSON_AddArrayToObject(caps, "features");
    cJSON_AddItemToArray(features, cJSON_CreateString("locomotion"));
    cJSON_AddItemToArray(features, cJSON_CreateString("lights"));
}

cJSON *freenove_car_command(const char *command, const cJSON *params) {
    const car_command_t *c = NULL;
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]) && !c; i++) {
        if (!strcmp(command, s_commands[i].name)) c = &s_commands[i];
    }
    if (!c) return NULL;
    if (params && !cJSON_IsObject(params) && !cJSON_IsNull(params)) {
        return car_error("invalid_param", "params must be an object");
    }
    if (cJSON_IsNull(params)) params = NULL;
    char why[400];
    if (!command_params_check(c->required, c->optional, params, why, sizeof(why))) {
        return car_error("invalid_param", why);
    }
    return c->run(params);
}

// Every 20 ms on the car's task: ends a drive whose time is up, and returns
// the light frame to show, if the effect moves.
static bool car_tick(uint8_t px[CAR_LEDS][3]) {
    car_lock();
    int64_t now = car_now_ms();
    if (s_car.moving && now >= s_car.until_ms) car_stop_locked("time_up");
    bool animated = car_light_animated(s_car.effect);
    if (animated) car_light_frame(now, px);
    car_unlock();
    return animated;
}

// From the session's task too (a lost link): the reason is kept only when a
// drive was cut short.
void freenove_car_stop(const char *reason) {
    car_lock();
    car_stop_locked(s_car.moving ? reason : s_car.stopped);
    car_unlock();
    car_hw_beep_off();
}

void freenove_car_status_light(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t px[CAR_LEDS][3];
    car_lock();
    s_car.status[0] = r;
    s_car.status[1] = g;
    s_car.status[2] = b;
    bool show = s_car.effect == FX_AUTO;
    if (show) car_light_frame(car_now_ms(), px);
    car_unlock();
    if (show) car_hw_lights(px);
}

bool freenove_car_battery(int *percent, int *millivolts) {
    car_lock();
    int mv = s_car.battery_mv;
    car_unlock();
    if (mv < CAR_BATTERY_PRESENT_MV) return false;
    *percent = car_battery_percent(mv);
    *millivolts = mv;
    return true;
}

// ---- Hardware ----

#define I2C_SDA GPIO_NUM_13
#define I2C_SCL GPIO_NUM_14
#define I2C_TIMEOUT_MS 50
#define PCA9685_ADDR 0x5F
#define PCF8574_ADDR 0x20
#define LED_GPIO GPIO_NUM_32            // also the battery divider
#define BATTERY_ADC ADC_CHANNEL_4       // GPIO32
#define LIGHT_ADC ADC_CHANNEL_5         // GPIO33
#define BATTERY_DIVIDER 4
#define BATTERY_EVERY_MS 30000
#define BUZZER_GPIO GPIO_NUM_2
#define SONIC_TRIG GPIO_NUM_12
#define SONIC_ECHO GPIO_NUM_15
#define SONIC_MAX_US 18000              // 3 m there and back
#define CONSOLE_UART UART_NUM_0

// PCA9685 registers.
#define PCA_MODE1 0x00
#define PCA_MODE2 0x01
#define PCA_LED0 0x06
#define PCA_PRESCALE 0xFE
#define PCA_SLEEP 0x10
#define PCA_AI 0x20
#define PCA_RESTART 0x80
#define PCA_OUTDRV 0x04
#define PCA_PRESCALE_50HZ 121           // 25 MHz / (4096 * 50 Hz) - 1

// Each motor's two inputs on the PCA9685, M1..M4; all in channels 8 to 15.
static const uint8_t MOTOR_CH[4][2] = { {15, 14}, {9, 8}, {12, 13}, {10, 11} };

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_lights_lock;   // the strip, or the ADC while it reads the battery
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_pca;
static i2c_master_dev_handle_t s_pcf;
static led_strip_handle_t s_strip;
static uint8_t s_shown[CAR_LEDS][3];
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static esp_timer_handle_t s_beep_timer;

static int64_t car_now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static void car_lock(void) {
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void car_unlock(void) {
    if (s_lock) xSemaphoreGive(s_lock);
}

static bool pca_write(uint8_t reg, const uint8_t *data, size_t n) {
    uint8_t buf[1 + 32];
    if (!s_pca || n > 32) return false;
    buf[0] = reg;
    memcpy(buf + 1, data, n);
    return i2c_master_transmit(s_pca, buf, 1 + n, I2C_TIMEOUT_MS) == ESP_OK;
}

static bool pca_write1(uint8_t reg, uint8_t value) {
    return pca_write(reg, &value, 1);
}

// One channel's four registers for `ticks` of 4096 on, from the start of
// the period; 0 sets the full-off bit.
static void pca_ticks(uint8_t out[4], int ticks) {
    out[0] = 0;
    out[1] = ticks >= 4096 ? 0x10 : 0;
    out[2] = ticks <= 0 || ticks >= 4096 ? 0 : ticks & 0xff;
    out[3] = ticks <= 0 ? 0x10 : ticks >= 4096 ? 0 : ticks >> 8;
}

// 50 Hz for the servos (the motors share it). The prescaler is set asleep;
// the oscillator then takes 500 us to start.
static bool pca_init(void) {
    if (!pca_write1(PCA_MODE1, PCA_SLEEP | PCA_AI) || !pca_write1(PCA_PRESCALE, PCA_PRESCALE_50HZ)
        || !pca_write1(PCA_MODE1, PCA_AI)) {
        return false;
    }
    esp_rom_delay_us(600);
    return pca_write1(PCA_MODE1, PCA_RESTART | PCA_AI) && pca_write1(PCA_MODE2, PCA_OUTDRV);
}

// All four motors in one write, channels 8 to 15.
static bool car_hw_motors(const int duty[4]) {
    uint8_t regs[8][4];
    for (int m = 0; m < 4; m++) {
        int d = duty[m];
        pca_ticks(regs[MOTOR_CH[m][0] - 8], d > 0 ? d : 0);
        pca_ticks(regs[MOTOR_CH[m][1] - 8], d < 0 ? -d : 0);
    }
    return pca_write(PCA_LED0 + 4 * 8, &regs[0][0], sizeof(regs));
}

static bool car_hw_servo(int channel, int pulse_us) {
    uint8_t regs[4];
    pca_ticks(regs, pulse_us * 4096 / 20000);
    return pca_write(PCA_LED0 + 4 * channel, regs, sizeof(regs));
}

static int car_hw_line(void) {
    uint8_t v;
    if (!s_pcf || i2c_master_receive(s_pcf, &v, 1, I2C_TIMEOUT_MS) != ESP_OK) return -1;
    return v & 7;
}

static void beep_off(void *arg) {
    (void)arg;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

static void car_hw_beep_off(void) {
    if (!s_beep_timer) return;
    esp_timer_stop(s_beep_timer);
    beep_off(NULL);
}

static bool car_hw_beep(int hz, int ms) {
    if (!s_beep_timer) return false;
    esp_timer_stop(s_beep_timer);
    if (ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1, hz) != ESP_OK) return false;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 512);   // half of 10 bits
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    return esp_timer_start_once(s_beep_timer, (uint64_t)ms * 1000) == ESP_OK;
}

static int car_hw_light_level(void) {
    int raw;
    return s_adc && adc_oneshot_read(s_adc, LIGHT_ADC, &raw) == ESP_OK ? raw : -1;
}

// A trigger pulse, then the echo's length: under 25 ms of busy waiting.
static int car_hw_distance_mm(void) {
    gpio_set_level(SONIC_TRIG, 1);
    esp_rom_delay_us(10);
    gpio_set_level(SONIC_TRIG, 0);
    int64_t sent = esp_timer_get_time();
    while (!gpio_get_level(SONIC_ECHO)) {
        if (esp_timer_get_time() - sent > 6000) return -1;
    }
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(SONIC_ECHO)) {
        if (esp_timer_get_time() - start > SONIC_MAX_US) return -1;
    }
    return (int)((esp_timer_get_time() - start) * 343 / 2000);   // 343 m/s, there and back
}

static esp_err_t strip_start(void) {
    led_strip_config_t strip = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = CAR_LEDS,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    return led_strip_new_rmt_device(&strip, &rmt, &s_strip);
}

static void strip_show(void) {
    for (int i = 0; i < CAR_LEDS && s_strip; i++) {
        led_strip_set_pixel(s_strip, i, s_shown[i][0], s_shown[i][1], s_shown[i][2]);
    }
    if (s_strip) led_strip_refresh(s_strip);
}

static void car_hw_lights(const uint8_t px[CAR_LEDS][3]) {
    if (!s_lights_lock) return;
    xSemaphoreTake(s_lights_lock, portMAX_DELAY);
    memcpy(s_shown, px, sizeof(s_shown));
    strip_show();
    xSemaphoreGive(s_lights_lock);
}

// GPIO32 is the lights' data line and the battery divider: the strip lets go
// of it for the few milliseconds the ADC needs, then shows its colours again.
static void battery_read(void) {
    xSemaphoreTake(s_lights_lock, portMAX_DELAY);
    if (s_strip) {
        led_strip_del(s_strip);
        s_strip = NULL;
    }
    adc_oneshot_chan_cfg_t chan = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    int sum = 0, n = 0;
    if (s_adc && adc_oneshot_config_channel(s_adc, BATTERY_ADC, &chan) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(2));
        for (int i = 0; i < 8; i++) {
            int raw, mv;
            if (adc_oneshot_read(s_adc, BATTERY_ADC, &raw) != ESP_OK) continue;
            if (!s_cali) {
                mv = raw * 3100 / 4095;   // uncalibrated, 12 dB attenuation
            } else if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
                continue;
            }
            sum += mv;
            n++;
        }
    }
    if (strip_start() != ESP_OK) {
        ESP_LOGE(TAG, "lights did not restart after the battery reading");
        s_strip = NULL;
    }
    strip_show();
    xSemaphoreGive(s_lights_lock);
    if (n) {
        car_lock();
        s_car.battery_mv = sum / n * BATTERY_DIVIDER;
        car_unlock();
    }
}

static void car_task(void *arg) {
    (void)arg;
    int64_t next_battery = 0;
    int64_t next_frame = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t px[CAR_LEDS][3];
        int64_t now = car_now_ms();
        if (car_tick(px) && now >= next_frame) {
            car_hw_lights(px);
            next_frame = now + 40;
        }
        if (now >= next_battery) {
            battery_read();
            next_battery = now + BATTERY_EVERY_MS;
        }
    }
}

// USB diagnostics: ">car chassis.drive {\"forward\":0.3,\"duration_ms\":300}"
// runs one of the car's commands and prints "@car <result>".
static void console_run(char *line) {
    char *json = strchr(line, ' ');
    if (json) *json++ = '\0';
    cJSON *params = json && *json ? cJSON_Parse(json) : NULL;
    cJSON *result = json && *json && !params ? car_error("invalid_json", "the parameters are not JSON")
                                             : freenove_car_command(line, params);
    if (!result) result = car_error("unsupported", "not one of the car's commands");
    char *text = cJSON_PrintUnformatted(result);
    printf("@car %s\n", text ? text : "{}");
    fflush(stdout);
    cJSON_free(text);
    cJSON_Delete(result);
    cJSON_Delete(params);
}

static void console_task(void *arg) {
    (void)arg;
    char line[256];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            line[n] = '\0';
            if (!strncmp(line, ">car ", 5)) console_run(line + 5);
            n = 0;
        } else if (n < sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
}

static void car_shutdown(void) {
    static const int zero[4] = {0, 0, 0, 0};
    car_hw_motors(zero);
    car_hw_beep_off();
}

bool freenove_car_init(void) {
    s_lock = xSemaphoreCreateMutex();
    s_lights_lock = xSemaphoreCreateMutex();
    if (!s_lock || !s_lights_lock) return false;

    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus, &s_bus) == ESP_OK) {
        i2c_device_config_t dev = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .scl_speed_hz = 100000 };
        if (i2c_master_probe(s_bus, PCA9685_ADDR, I2C_TIMEOUT_MS) == ESP_OK) {
            dev.device_address = PCA9685_ADDR;
            i2c_master_bus_add_device(s_bus, &dev, &s_pca);
        }
        if (i2c_master_probe(s_bus, PCF8574_ADDR, I2C_TIMEOUT_MS) == ESP_OK) {
            dev.device_address = PCF8574_ADDR;
            if (i2c_master_bus_add_device(s_bus, &dev, &s_pcf) == ESP_OK) {
                uint8_t inputs = 0xff;   // quasi-bidirectional: high to read
                i2c_master_transmit(s_pcf, &inputs, 1, I2C_TIMEOUT_MS);
            }
        }
    }
    // The PCA9685 keeps its outputs through a reset of the ESP32, so the
    // wheels are stopped before anything else.
    static const int zero[4] = {0, 0, 0, 0};
    s_car.base_ok = pca_init() && car_hw_motors(zero);
    if (s_car.base_ok) {
        car_hw_servo(0, car_servo_us(s_car.pan));
        car_hw_servo(1, car_servo_us(s_car.tilt));
    }
    ESP_LOGI(TAG, "PCA9685 at 0x%02X: %s; line sensor PCF8574 at 0x%02X: %s", PCA9685_ADDR,
             s_car.base_ok ? "ready, motors stopped, servos at 90" : "not answering", PCF8574_ADDR,
             s_pcf ? "ready" : "not answering");
    esp_register_shutdown_handler(car_shutdown);

    gpio_config_t trig = { .pin_bit_mask = 1ULL << SONIC_TRIG, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&trig);
    gpio_set_level(SONIC_TRIG, 0);
    gpio_config_t echo = { .pin_bit_mask = 1ULL << SONIC_ECHO, .mode = GPIO_MODE_INPUT,
                           .pull_down_en = GPIO_PULLDOWN_ENABLE };
    gpio_config(&echo);

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1, .freq_hz = 2000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t channel = {
        .gpio_num = BUZZER_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_1, .duty = 0,
    };
    esp_timer_create_args_t beep = { .callback = beep_off, .name = "car_beep" };
    if (ledc_timer_config(&timer) != ESP_OK || ledc_channel_config(&channel) != ESP_OK
        || esp_timer_create(&beep, &s_beep_timer) != ESP_OK) {
        ESP_LOGW(TAG, "buzzer unavailable");
        s_beep_timer = NULL;
    }

    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_chan_cfg_t chan = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_new_unit(&unit, &s_adc) == ESP_OK) {
        adc_oneshot_config_channel(s_adc, LIGHT_ADC, &chan);
        adc_cali_line_fitting_config_t cali = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12,
                                                .bitwidth = ADC_BITWIDTH_DEFAULT };
        if (adc_cali_create_scheme_line_fitting(&cali, &s_cali) != ESP_OK) s_cali = NULL;
    } else {
        s_adc = NULL;
    }

    battery_read();   // starts the strip too
    ESP_LOGI(TAG, "lights: %d WS2812 on GPIO%d %s; battery %d mV", CAR_LEDS, LED_GPIO,
             s_strip ? "ready" : "failed", s_car.battery_mv);

    s_car.ticking = xTaskCreate(car_task, "car", 3072, NULL, 6, NULL) == pdPASS;
    if (uart_driver_install(CONSOLE_UART, 512, 0, 0, NULL, 0) != ESP_OK
        || xTaskCreate(console_task, "car_console", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "USB diagnostics unavailable");
    }
    return s_strip && s_car.ticking;
}
