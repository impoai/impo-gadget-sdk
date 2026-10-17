/* ESP-IDF esp_timer_get_time() on ARCS: microseconds since boot, at the 1 ms tick's resolution. */
#pragma once

#include <stdint.h>

#include "sdkconfig.h"
#include "platform_arcs.h"

static inline int64_t esp_timer_get_time(void)
{
    return (int64_t)plat_uptime_ms() * 1000;
}
