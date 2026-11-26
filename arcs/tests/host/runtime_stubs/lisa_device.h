#pragma once
#include <stdbool.h>
typedef struct { int id; } lisa_device_t;
#define LISA_DEVICE_OK 0
lisa_device_t *lisa_device_get(const char *name);
bool lisa_device_ready(lisa_device_t *dev);
