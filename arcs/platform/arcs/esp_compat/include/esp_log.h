/* ESP-IDF esp_log.h on ARCS: the ESP_LOGx macros print through LISA_LOGx (easylogger, reaches ADB). */
#pragma once

#include "sdkconfig.h"
#include "lisa_log.h"

#define ESP_LOGE(tag, fmt, ...) LISA_LOGE(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) LISA_LOGW(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) LISA_LOGI(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) LISA_LOGD(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) LISA_LOGD(tag, fmt, ##__VA_ARGS__)
