// SPDX-License-Identifier: MIT
/*
 * Freenove 4WD Car Kit for ESP32 (FNK0053): an ESP32-WROVER (ESP32-D0WD-V3,
 * 4 MB flash, 8 MB PSRAM) on Freenove's car board, with a CH340 USB bridge.
 * Facts from https://github.com/Freenove/Freenove_4WD_Car_Kit_for_ESP32
 * (Tutorial.pdf, "Pins of the Car" and chapters 1 and 2, and the sketches
 * below); the code is this firmware's own.
 *
 * What this drives, the wheels, the head's two servos, the distance sensor,
 * the camera and the battery reading:
 * - I2C on GPIO13 (SDA) and GPIO14 (SCL) to a PCA9685 at 0x5F, run at
 *   50 Hz as Freenove does (Freenove_4WD_Car_For_ESP32.h). The motors' two
 *   inputs: M1 15/14, M2 9/8, M3 12/13, M4 10/11. M1 and M2 are the left
 *   wheels, M3 and M4 the right, and a positive duty on the first input
 *   drives forward (01.1_Car_Move_and_Turn). Below a duty of 1600 of 4095
 *   the gear motors stall (Motor_Move()). Servo 1 on channel 0 pans the
 *   head: 90 is straight ahead, larger looks left (02.2_Ultrasonic_Ranging_Car,
 *   get_distance()). Servo 2 on channel 1 tilts it; Freenove keeps it at
 *   80 to 180 (06.3, Servo_2_Angle()). Both start at 90.
 * - GPIO32: the battery divider, a quarter of the voltage of the two 18650
 *   cells (8.4 V full; Tutorial.pdf 2.4 and 01.4_Battery_level,
 *   batteryCoefficient 4).
 * - GPIO12 and GPIO15: the HC-SR04 ultrasonic trigger and echo, up to 3 m
 *   (02.1_Ultrasonic_Ranging, Get_Sonar()). Both are strapping pins, left low.
 *   It looks where the head points.
 * - The camera on the head (an OV2640 or an OV3660, which esp32-camera tells
 *   apart), through the camera connector and an FPC cable to
 *   the WROVER's camera socket (Tutorial.pdf "Pins of the Car"): D0..D7 on
 *   GPIO4, 5, 18, 19, 36, 39, 34, 35, VSYNC 25, HREF 23, PCLK 22, XCLK 21,
 *   SCCB on GPIO26 (SDA) and 27 (SCL), no power-down or reset line. Link's
 *   camera.capture takes its photos (components/camera).
 * - GPIO0: the BOOT button, shared with the infrared receiver (05.1).
 *
 * Future work, for parts not wired on this car yet (no commands until they
 * are, so the agent is never offered one that does nothing):
 * - the 12 WS2812 lights (GRB) on GPIO32, which they share with the battery
 *   divider, so the light driver must let go of the pin for each reading
 *   (01.6_WS2812);
 * - the line-tracking sensors, a PCF8574 at 0x20, bits 0 to 2 left, middle,
 *   right (04.1 Track_Car());
 * - the photoresistors on GPIO33 (03.1, 03.2 Light_Car());
 * - the passive buzzer on GPIO2 (01.3);
 * - the VK16K33 LED matrix at 0x71.
 *
 * The wheels and servos run off the cells: on USB alone the commands work, the PCA9685
 * answers, and nothing moves. A drive is always bounded (10 s at most) and
 * ends on its own, on chassis.stop, when the Link session drops, and before
 * a restart.
 */
#include "freenove_4wd_car.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "camera_dvp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/base64.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "link.car";

// ---- Commands (host-tested) ----
// From here to "---- Hardware" builds on the host too
// (tests/freenove_4wd_car_harness.c stands in for the car_hw_* calls).

#define CAR_MAX_MS 10000
#define CAR_DEFAULT_MS 500
#define CAR_MOTOR_MIN 1600
#define CAR_MOTOR_MAX 4095
#define CAR_DEFAULT_SPEED 0.5
#define CAR_BATTERY_PRESENT_MV 5000   // under this there is no charged cell
#define CAR_BATTERY_EMPTY_MV 6000
#define CAR_BATTERY_FULL_MV 8400

// Hardware, defined under "---- Hardware" (or by the host harness).
static bool car_hw_motors(const int duty[4]);   // M1..M4, -4095..4095
static bool car_hw_servo(int channel, int pulse_us);
static int car_hw_battery_mv(void);   // read now; -1 if the ADC did not answer
static int car_hw_distance_mm(void);  // -1 without an echo within 3 m
static int64_t car_now_ms(void);
static void car_lock(void);     // no-ops until freenove_car_init()
static void car_unlock(void);

static struct {
    bool ticking;            // the task that ends drives runs: nothing moves without it
    bool base_ok;            // the PCA9685 took the last write
    bool moving;
    double forward, turn;
    int64_t until_ms;
    const char *stopped;     // why the last drive ended
    int pan, tilt;           // the head's servos, in degrees
} s_car = { .stopped = "not_started", .pan = 90, .tilt = 90 };

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

// The firmware registers only command names; the product's manifest tells the
// agent the ranges, and these checks keep the car safe whatever arrives.
// fallback when key is left out; false when it is not a number in lo..hi
// (a whole one, when whole).
static bool car_param(const cJSON *params, const char *key, double fallback, double lo, double hi,
                      bool whole, double *out) {
    const cJSON *v = cJSON_GetObjectItem(params, key);
    if (!v) {
        *out = fallback;
        return true;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi ||
        (whole && v->valuedouble != (double)v->valueint)) {
        return false;
    }
    *out = v->valuedouble;
    return true;
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
    int mv = car_hw_battery_mv();
    if (mv < 0) {
        cJSON_AddNullToObject(p, "battery_v");
        return;
    }
    cJSON_AddNumberToObject(p, "battery_v", round(mv / 10.0) / 100.0);
    if (mv < CAR_BATTERY_PRESENT_MV) {
        cJSON_AddStringToObject(p, "warning",
                                "No charged battery (or the power switch is off): the wheels run off "
                                "the two 18650 cells and will not move on USB alone.");
    } else if (mv < CAR_BATTERY_EMPTY_MV + 400) {
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

static bool car_duration(const cJSON *params, double *ms) {
    return car_param(params, "duration_ms", CAR_DEFAULT_MS, 1, CAR_MAX_MS, true, ms);
}

static cJSON *cmd_drive(const cJSON *params) {
    double forward, turn, ms;
    if (!car_param(params, "forward", 0, -1, 1, false, &forward) ||
        !car_param(params, "turn", 0, -1, 1, false, &turn)) {
        return car_error("invalid_param", "forward and turn must be -1 to 1");
    }
    if (!car_duration(params, &ms)) {
        return car_error("invalid_param", "duration_ms must be a whole number, 1 to 10000");
    }
    return car_drive(forward, turn, (int)ms);
}

static cJSON *cmd_move(const cJSON *params) {
    const char *direction = cJSON_GetStringValue(cJSON_GetObjectItem(params, "direction"));
    double speed, ms;
    if (!car_param(params, "speed", CAR_DEFAULT_SPEED, 0.1, 1, false, &speed)) {
        return car_error("invalid_param", "speed must be 0.1 to 1");
    }
    if (!car_duration(params, &ms)) {
        return car_error("invalid_param", "duration_ms must be a whole number, 1 to 10000");
    }
    static const struct { const char *name; int forward, turn; } moves[] = {
        {"forward", 1, 0}, {"back", -1, 0}, {"left", 0, -1}, {"right", 0, 1}, {"stop", 0, 0},
    };
    for (size_t i = 0; direction && i < sizeof(moves) / sizeof(moves[0]); i++) {
        if (!strcmp(direction, moves[i].name)) {
            return car_drive(moves[i].forward * speed, moves[i].turn * speed, (int)ms);
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
    car_unlock();
    return car_ok(payload);
}

// 0..180 degrees to 500..2500 us, the range Freenove maps its servos over.
static int car_servo_us(int angle) {
    return 500 + angle * 2000 / 180;
}

static cJSON *cmd_servo(const cJSON *params) {
    // Below a tilt of 80 the head runs into the car (Freenove's Servo_2_Angle()).
    double pan, tilt;
    if (!car_param(params, "pan", -1, 0, 180, true, &pan) ||
        !car_param(params, "tilt", -1, 80, 180, true, &tilt)) {
        return car_error("invalid_param", "pan must be a whole number 0 to 180, tilt 80 to 180");
    }
    if (pan < 0 && tilt < 0) return car_error("invalid_param", "give pan, tilt or both");
    bool ok = true;
    cJSON *payload = cJSON_CreateObject();
    car_lock();
    if (pan >= 0 && (ok = car_hw_servo(0, car_servo_us((int)pan)))) s_car.pan = (int)pan;
    if (ok && tilt >= 0 && (ok = car_hw_servo(1, car_servo_us((int)tilt)))) s_car.tilt = (int)tilt;
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
    return car_ok(payload);
}

typedef struct {
    const char *name;
    cJSON *(*run)(const cJSON *params);
} car_command_t;

// What the agent reads about these is in the product's manifest
// (tools/freenove_4wd_car_manifest.py).
static const car_command_t s_commands[] = {
    { "chassis.drive",   cmd_drive },
    { "chassis.move",    cmd_move },
    { "chassis.stop",    cmd_stop },
    { "chassis.status",  cmd_status },
    { "chassis.sense",   cmd_sense },
    { "servo.set_angle", cmd_servo },
};

void freenove_car_describe(cJSON *commands) {
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        cJSON_AddObjectToObject(commands, s_commands[i].name);
    }
}

void freenove_car_capabilities(cJSON *params) {
    cJSON *caps = cJSON_AddObjectToObject(params, "capabilities");
    cJSON_AddBoolToObject(caps, "battery", true);
    cJSON_AddBoolToObject(caps, "buttons", true);
    cJSON *features = cJSON_AddArrayToObject(caps, "features");
    cJSON_AddItemToArray(features, cJSON_CreateString("locomotion"));
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
    return c->run(params);
}

// Every 20 ms on the car's task: ends a drive whose time is up.
static void car_tick(void) {
    car_lock();
    if (s_car.moving && car_now_ms() >= s_car.until_ms) car_stop_locked("time_up");
    car_unlock();
}

// From the session's task too (a lost link): the reason is kept only when a
// drive was cut short.
void freenove_car_stop(const char *reason) {
    car_lock();
    car_stop_locked(s_car.moving ? reason : s_car.stopped);
    car_unlock();
}

bool freenove_car_battery(int *percent, int *millivolts) {
    int mv = car_hw_battery_mv();
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
#define BATTERY_ADC ADC_CHANNEL_4       // GPIO32
#define BATTERY_DIVIDER 4
#define CAM_SDA GPIO_NUM_26
#define CAM_SCL GPIO_NUM_27
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
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_pca;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

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

// 50 Hz, as Freenove runs it (its servos share the chip). The prescaler is
// set asleep; the oscillator then takes 500 us to start.
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

static int car_hw_battery_mv(void) {
    int sum = 0, n = 0;
    for (int i = 0; s_adc && i < 8; i++) {
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
    return n ? sum / n * BATTERY_DIVIDER : -1;
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

static void car_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        car_tick();
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

// USB diagnostics: ">cam" takes a photo and prints it as "@cam begin <bytes>",
// base64 lines and "@cam end", or "@cam error <code>".
static void console_photo(void) {
    camera_frame_t frame;
    esp_err_t err = camera_capture(&frame);
    if (err != ESP_OK) {
        printf("@cam error %s\n", esp_err_to_name(err));
        fflush(stdout);
        return;
    }
    printf("@cam begin %u %dx%d\n", (unsigned)frame.len, frame.width, frame.height);
    for (size_t at = 0; at < frame.len; at += 48) {
        unsigned char line[65];
        size_t n = 0;
        size_t chunk = frame.len - at < 48 ? frame.len - at : 48;
        mbedtls_base64_encode(line, sizeof(line), &n, frame.jpeg + at, chunk);
        line[n] = '\0';
        printf("%s\n", line);
    }
    printf("@cam end\n");
    fflush(stdout);
    camera_release(&frame);
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
            else if (!strcmp(line, ">cam")) console_photo();
            n = 0;
        } else if (n < sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
}

static void car_shutdown(void) {
    static const int zero[4] = {0, 0, 0, 0};
    car_hw_motors(zero);
}

bool freenove_car_init(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;

    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus, &s_bus) == ESP_OK
        && i2c_master_probe(s_bus, PCA9685_ADDR, I2C_TIMEOUT_MS) == ESP_OK) {
        i2c_device_config_t dev = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = PCA9685_ADDR,
                                    .scl_speed_hz = 100000 };
        i2c_master_bus_add_device(s_bus, &dev, &s_pca);
    }
    // The PCA9685 keeps its outputs through a reset of the ESP32, so the
    // wheels are stopped before anything else.
    static const int zero[4] = {0, 0, 0, 0};
    s_car.base_ok = pca_init() && car_hw_motors(zero);
    if (s_car.base_ok) {
        car_hw_servo(0, car_servo_us(s_car.pan));
        car_hw_servo(1, car_servo_us(s_car.tilt));
    }
    ESP_LOGI(TAG, "PCA9685 at 0x%02X: %s", PCA9685_ADDR,
             s_car.base_ok ? "ready, motors stopped, head at 90" : "not answering");
    esp_register_shutdown_handler(car_shutdown);

    // The camera's own SCCB bus; esp32-camera finds it by its port.
    i2c_master_bus_config_t cam_bus = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = CAM_SDA,
        .scl_io_num = CAM_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t cam_i2c;
    if (i2c_new_master_bus(&cam_bus, &cam_i2c) == ESP_OK) {
        static const camera_dvp_config_t cam = {
            .name = "the Freenove car's head camera",
            .xclk = GPIO_NUM_21, .pclk = GPIO_NUM_22, .vsync = GPIO_NUM_25, .href = GPIO_NUM_23,
            .d = { GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_18, GPIO_NUM_19,
                   GPIO_NUM_36, GPIO_NUM_39, GPIO_NUM_34, GPIO_NUM_35 },
            .i2c_port = I2C_NUM_1,
        };
        camera_register(camera_dvp(&cam));
    } else {
        ESP_LOGW(TAG, "camera bus did not start: no camera");
    }

    gpio_config_t trig = { .pin_bit_mask = 1ULL << SONIC_TRIG, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&trig);
    gpio_set_level(SONIC_TRIG, 0);
    gpio_config_t echo = { .pin_bit_mask = 1ULL << SONIC_ECHO, .mode = GPIO_MODE_INPUT,
                           .pull_down_en = GPIO_PULLDOWN_ENABLE };
    gpio_config(&echo);

    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_chan_cfg_t chan = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_new_unit(&unit, &s_adc) == ESP_OK
        && adc_oneshot_config_channel(s_adc, BATTERY_ADC, &chan) == ESP_OK) {
        adc_cali_line_fitting_config_t cali = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12,
                                                .bitwidth = ADC_BITWIDTH_DEFAULT };
        if (adc_cali_create_scheme_line_fitting(&cali, &s_cali) != ESP_OK) s_cali = NULL;
    } else {
        s_adc = NULL;
    }
    ESP_LOGI(TAG, "battery %d mV", car_hw_battery_mv());

    s_car.ticking = xTaskCreate(car_task, "car", 3072, NULL, 6, NULL) == pdPASS;
    if (uart_driver_install(CONSOLE_UART, 512, 0, 0, NULL, 0) != ESP_OK
        || xTaskCreate(console_task, "car_console", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "USB diagnostics unavailable");
    }
    return s_car.ticking;
}
