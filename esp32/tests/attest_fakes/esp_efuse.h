// SPDX-License-Identifier: MIT
#pragma once

typedef enum { EFUSE_BLK_KEY4 = 8, EFUSE_BLK_KEY5 = 9 } esp_efuse_block_t;
typedef enum {
    ESP_EFUSE_KEY_PURPOSE_USER = 0,
    ESP_EFUSE_KEY_PURPOSE_HMAC_UP = 8,
} esp_efuse_purpose_t;

esp_efuse_purpose_t esp_efuse_get_key_purpose(esp_efuse_block_t block);
