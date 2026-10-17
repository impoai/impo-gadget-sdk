// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_EFUSE_FACTORY } esp_mac_type_t;

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type);
