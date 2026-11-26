/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include "../../platform/arcs/plat_audio.c"
static lisa_device_t dev;
static unsigned starts, stops, wake_frames, voice_frames;
static int fail_config, fail_start;
lisa_device_t *lisa_device_get(const char *n) { (void)n; return &dev; }
bool lisa_device_ready(lisa_device_t *d) { return d==&dev; }
int lisa_audio_play_config(lisa_device_t *d,const lisa_audio_play_config_t *c) { (void)d;(void)c;return 0; }
int lisa_audio_play_start(lisa_device_t *d) { (void)d;return 0; }
int lisa_audio_play_write(lisa_device_t *d,const int16_t *p,uint32_t n) { (void)d;(void)p;return n; }
int lisa_audio_record_config(lisa_device_t *d,const lisa_audio_record_config_t *c) { (void)d;(void)c;return fail_config; }
int lisa_audio_record_start(lisa_device_t *d) { (void)d;starts++;return fail_start; }
int lisa_audio_record_stop(lisa_device_t *d) { (void)d;stops++;return 0; }
int lisa_audio_register_callback(lisa_device_t *d,void (*f)(const lisa_audio_event_t *,void *),void *ctx)
{ (void)d;(void)ctx;assert(f==on_audio);return 0; }
void test_log(const char *t,const char *f,...) { (void)t;(void)f; }
static void wake_cb(const int16_t *p,size_t n) { (void)p;wake_frames+=n; }
static void voice_cb(const int16_t *p,size_t n) { (void)p;voice_frames+=n; }
static void frame(void) { int16_t p[256]={0};lisa_audio_event_t e={p,256};on_audio(&e,NULL); }
int main(void)
{
    assert(plat_audio_start()==0);
    assert(plat_audio_wake_start(wake_cb)==0 && starts==1);
    frame();assert(wake_frames==256 && voice_frames==0);
    assert(plat_audio_mic_start(voice_cb)==0 && starts==1);
    frame();assert(wake_frames==256 && voice_frames==256);
    assert(plat_audio_mic_start(voice_cb)==-EBUSY);
    plat_audio_mic_stop();assert(stops==0);frame();assert(wake_frames==512);
    assert(plat_audio_mic_start(voice_cb)==0);
    plat_audio_wake_stop();assert(stops==0);frame();assert(voice_frames==512);
    plat_audio_mic_stop();assert(stops==1);
    plat_audio_mic_stereo(true);assert(plat_audio_wake_start(wake_cb)==-EBUSY);
    assert(plat_audio_mic_start(voice_cb)==0);assert(plat_audio_wake_start(wake_cb)==-EBUSY);
    plat_audio_mic_stop();plat_audio_mic_stereo(false);
    fail_config=-1;assert(plat_audio_wake_start(wake_cb)==-EIO && !s_wake_cb && !s_recording);
    fail_config=0;fail_start=-1;assert(plat_audio_mic_start(voice_cb)==-EIO && !s_mic_cb && !s_recording);
    fail_start=0;assert(plat_audio_wake_start(wake_cb)==0);plat_audio_wake_stop();
    assert(!s_recording);
    puts("Microphone: exclusive routing, uninterrupted handover, stereo exclusion and error rollback passed");
}
