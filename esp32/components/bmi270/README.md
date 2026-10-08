# bmi270

Espressif's BMI270 driver, copied from
[esp-bsp](https://github.com/espressif/esp-bsp/tree/master/components/sensors/bmi270)
v1.1.0 (Apache-2.0, see LICENSE). Only `bmi270_sensor_hub.c` is left out: it
registers with `sensor_hub`, which needs the legacy `i2c_bus`, while the boards
here drive I2C with `driver/i2c_master.h`, as this driver does. Nothing else
is changed except that `bmi270_create` also accepts the BMI260 (chip ID
0x27), uploading that part's own configuration file (`src/bmi260_config.h`,
Bosch's v2.46.1 via ChromiumOS EC, BSD-3-Clause, see LICENSE.bmi260). Newer
ESP-SparkBots carry a BMI260; either way the board reads its accelerometer and
gyroscope through this driver.
