/* ESP-IDF esp_random() on ARCS: 32 bits from the hardware TRNG (plat_random). */
#pragma once

#include <stdint.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t esp_random(void);

#ifdef __cplusplus
}
#endif
