#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
int xTaskCreate(void (*fn)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
void vTaskDelay(unsigned ms);

void vTaskDelete(TaskHandle_t task);
