// SPDX-License-Identifier: MIT
// Freenove 4WD Car Kit for ESP32 (FNK0053): its wheels and battery, as Link
// commands. See freenove_4wd_car.c for the pin map and the parts to come.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

// Starts the I2C bus, stops the motors, reads the battery and starts the
// task that ends drives on time. Call first in app_main, so a car that reset
// mid-drive stops at once. False if the task could not start (the car then
// refuses to drive); a missing PCA9685 is not fatal (the commands then say
// the motor driver is not answering).
bool freenove_car_init(void);

// Adds the car's commands to link.register's commands_v2.
void freenove_car_describe(cJSON *commands);
// Adds link.register's capabilities summary.
void freenove_car_capabilities(cJSON *params);
// Runs one of the car's commands: a {"ok", "payload"} or {"ok": false,
// "error"} result, or NULL when `command` is not the car's. Returns at once.
cJSON *freenove_car_command(const char *command, const cJSON *params);

// Stops the wheels now. `reason` is reported by chassis.status
// ("link_lost", for one).
void freenove_car_stop(const char *reason);

// The last battery reading, for device.health. False with no charged battery
// fitted (or the power switch off): the board then reads well under 5 V.
bool freenove_car_battery(int *percent, int *millivolts);
