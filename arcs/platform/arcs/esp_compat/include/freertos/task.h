/*
 * ESP-IDF's freertos/task.h on ARCS. Three differences from ESP-IDF are bridged
 * here, for the files that include this header (the shared link from esp32/main) only:
 *
 * - ESP-IDF's StackType_t is one byte, so its xTaskCreate() takes the stack in
 *   bytes; the SDK's FreeRTOS takes words. The depth is converted, so
 *   NOISE_CTRL_STACK 12288 is a 12 KB stack as on the ESP32.
 * - Priorities are capped at ESP_COMPAT_MAX_TASK_PRIORITY, below the audio
 *   driver's dispatch task (CONFIG_LISA_AUDIO_DISPATCH_TASK_PRIORITY 5), which
 *   must never wait behind the link (upstream starts noise_ctrl at 5).
 * - vTaskDelay() also checks in with the task supervision of
 *   platform/arcs/plat_watchdog.c when the calling task is supervised
 *   (esp_compat_wdt_supervise): a link task sleeping in its reconnect delay is
 *   alive, one stuck elsewhere is not.
 */
#pragma once

#include "sdkconfig.h"
#include <FreeRTOS.h>
#include <task.h>

#define ESP_COMPAT_MAX_TASK_PRIORITY 4

#ifdef __cplusplus
extern "C" {
#endif

void esp_compat_task_delay(TickType_t ticks);
void esp_compat_wdt_alive(const char *where);

#ifdef __cplusplus
}
#endif

#define xTaskCreate(fn, name, stack_bytes, arg, prio, handle)                                          \
    xTaskCreate((fn), (name), (configSTACK_DEPTH_TYPE)((stack_bytes) / sizeof(StackType_t)), (arg), \
                ((prio) > ESP_COMPAT_MAX_TASK_PRIORITY ? ESP_COMPAT_MAX_TASK_PRIORITY : (prio)), (handle))
#define vTaskDelay(ticks) esp_compat_task_delay(ticks)
