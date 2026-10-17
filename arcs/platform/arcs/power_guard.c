/*
 * Power latch for boards that hold POWER_EN after the key is released.
 * Runs before the application so the board stays on once boot hands over.
 */
#include <stdbool.h>

#include "sys_init.h"
#include "board.h"
#include "lisa_gpio.h"
#include "power_manager.h"
#include "uboot_features_api.h"

#define TAG "power_guard"
#include "lisa_log.h"

static int power_up_guard(void)
{
    power_config_t cfg = {0};
    power_init(&cfg);

    /* A newer boot already guards the long press in stage 0; older boots leave it to the app. */
    if (!uboot_features_has(UBOOT_FEATURE_POWER_GUARD)) {
        if (!power_wait_settle()) {
            power_shutdown();
            return 0;
        }
        lisa_gpio_configure(lisa_device_get("gpiob"), LED_PIN,
                            LISA_GPIO_OUTPUT | LISA_GPIO_OUTPUT_INIT_LOW);
    }
    return 0;
}

SYS_INIT(power_up_guard, SYS_INIT_LEVEL_PRE_APPLICATION, 0);
