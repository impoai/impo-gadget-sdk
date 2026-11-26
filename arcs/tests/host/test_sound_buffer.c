/* Exercise the real SDK sound producer/output task with deterministic RTOS steps. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "task.h"
#include "gadget_sound.h"

static void (*output_task)(void *);
static jmp_buf step;
static int16_t played[256];
static size_t played_n;
static bool amp_on;

void test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
int lisa_kv_get_int(const char *key, int *v) { (void)key; (void)v; return -1; }
int lisa_kv_set_int(const char *key, int v) { (void)key; (void)v; return 0; }
int plat_audio_start(void) { return 0; }
int plat_watchdog_register(const char *name, unsigned ms) { (void)name; (void)ms; return 0; }
void plat_watchdog_checkin(int id) { (void)id; }
int xTaskCreate(void (*fn)(void *), const char *n, unsigned s, void *a, unsigned p, TaskHandle_t *h)
{
    (void)n; (void)s; (void)a; (void)p; (void)h;
    output_task = fn;
    return pdPASS;
}
void vTaskDelay(unsigned ms)
{
    if (ms == 10) longjmp(step, 1); /* Yield only on an idle/buffering iteration. */
    assert(ms == 20); /* Amp settling completes without discarding the task's block. */
}
int plat_audio_write(const int16_t *pcm, size_t n)
{
    assert(n == 256);
    memcpy(played, pcm, n * sizeof(*pcm));
    played_n += n;
    longjmp(step, 1);
}
static void set_amp(bool on) { amp_on = on; }
static void output_step(void)
{
    if (setjmp(step) == 0) output_task(NULL);
}
static void drain_block(void)
{
    for (int i = 0; i < 4 && played_n == 0; i++) output_step();
}

int main(void)
{
    assert(gadget_sound_start(set_amp) == 0);
    assert(gadget_sound_set_volume(100) == 0);
    int16_t pcm[256];
    for (int i = 0; i < 256; i++) pcm[i] = (int16_t)(i * 30);

    uint32_t g = gadget_sound_begin_buffered("test");
    assert(gadget_sound_write(g, pcm, 256, 0) == 0);
    size_t room = gadget_sound_room();
    for (int i = 0; i < 20; i++) output_step();
    assert(played_n == 0 && !amp_on);
    assert(gadget_sound_state() == GADGET_SOUND_FETCHING);
    assert(gadget_sound_room() == room); /* No samples consumed during network gaps. */
    gadget_sound_end(g, NULL);
    drain_block();
    assert(played_n == 256 && amp_on);
    for (int i = 0; i < 256; i++) assert(played[i] == (int16_t)((pcm[i] * 32767) >> 15));

    played_n = 0;
    g = gadget_sound_begin_buffered("truncated");
    assert(gadget_sound_write(g, pcm, 256, 0) == 0);
    gadget_sound_end(g, "connection dropped");
    for (int i = 0; i < 4; i++) output_step();
    assert(played_n == 0 && gadget_sound_state() == GADGET_SOUND_FAILED);
    assert(strcmp(gadget_sound_error(), "connection dropped") == 0);

    g = gadget_sound_begin_buffered("cancelled");
    assert(gadget_sound_write(g, pcm, 256, 0) == 0);
    gadget_sound_stop();
    assert(gadget_sound_write(g, pcm, 256, 0) == -1);
    output_step();
    assert(played_n == 0 && gadget_sound_state() == GADGET_SOUND_IDLE);
    uint32_t newer = gadget_sound_begin("local tone");
    assert(gadget_sound_write(g, pcm, 256, 0) == -1);
    assert(gadget_sound_write(newer, pcm, 256, 0) == 0);
    assert(gadget_sound_state() == GADGET_SOUND_PLAYING); /* Local sounds need no download. */

    played_n = 0;
    g = gadget_sound_begin_buffered("long reply");
    for (int i = 0; i < 25 * GADGET_SOUND_RATE / 256 + 1; i++)
        assert(gadget_sound_write(g, pcm, 256, 0) == 0);
    assert(gadget_sound_room() > 0); /* Release before filling the existing 30 s ring. */
    drain_block();
    assert(played_n > 0 && gadget_sound_state() == GADGET_SOUND_PLAYING);
    puts("Sound buffer: delayed start, ordered PCM, failed download, cancellation and long reply passed");
    return 0;
}
