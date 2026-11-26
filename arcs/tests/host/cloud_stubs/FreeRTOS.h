#pragma once
#include <stddef.h>
#include <stdint.h>
typedef unsigned StackType_t;
typedef unsigned TickType_t;
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY UINT32_MAX
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
