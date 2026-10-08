# Bosch BMI270 driver from espressif/esp-bsp (components/sensors/bmi270,
# v1.1.0, Apache-2.0), without its sensor_hub registration: that pulls in the
# legacy I2C driver, and the boards here use the new one.
idf_component_register(SRCS "src/bmi270.c" INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                       REQUIRES esp_driver_i2c esp_driver_spi)
