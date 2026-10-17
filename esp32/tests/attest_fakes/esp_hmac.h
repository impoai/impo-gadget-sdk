// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum { HMAC_KEY4 = 4, HMAC_KEY5 = 5 } hmac_key_id_t;

esp_err_t esp_hmac_calculate(hmac_key_id_t key_id, const void *message,
                             size_t message_len, uint8_t *hmac);
