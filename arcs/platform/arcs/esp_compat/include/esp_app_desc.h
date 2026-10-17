/* ESP-IDF esp_app_desc.h on ARCS: the version is plat_firmware_version() ("0.1.0"). */
#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char version[32];
    char project_name[32];
} esp_app_desc_t;

const esp_app_desc_t *esp_app_get_description(void);

#ifdef __cplusplus
}
#endif
