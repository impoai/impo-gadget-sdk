#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lisa_device.h"
#define LISA_AUDIO_RATE_16K 16000
#define LISA_AUDIO_CH_LEFT 1
#define LISA_AUDIO_CH_STEREO 3
#define LISA_AUDIO_BIT_16 16
typedef struct { unsigned sample_rate, channels, sample_bits; } test_format_t;
typedef struct { int analog_gain, digital_gain; } test_gain_t;
typedef struct { test_format_t format; test_gain_t gain; unsigned buffer_count, buffer_samples; } lisa_audio_play_config_t;
typedef struct { test_format_t format; test_gain_t gain; bool differential_input, enable_hpf; } lisa_audio_record_config_t;
typedef struct { const void *record_buffer; unsigned record_samples; } lisa_audio_event_t;
int lisa_audio_play_config(lisa_device_t *, const lisa_audio_play_config_t *);
int lisa_audio_play_start(lisa_device_t *);
int lisa_audio_play_write(lisa_device_t *,const int16_t *,uint32_t);
int lisa_audio_record_config(lisa_device_t *,const lisa_audio_record_config_t *);
int lisa_audio_record_start(lisa_device_t *);
int lisa_audio_record_stop(lisa_device_t *);
int lisa_audio_register_callback(lisa_device_t *,void (*)(const lisa_audio_event_t *,void *),void *);
