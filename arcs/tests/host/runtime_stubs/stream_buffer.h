#pragma once
#include "FreeRTOS.h"
typedef void *StreamBufferHandle_t;
StreamBufferHandle_t xStreamBufferCreate(size_t, size_t);
int xStreamBufferReset(StreamBufferHandle_t);
size_t xStreamBufferSend(StreamBufferHandle_t, const void *, size_t, unsigned);
size_t xStreamBufferReceive(StreamBufferHandle_t, void *, size_t, unsigned);
