/*
 * System watchdog (wdt0, 32 kHz) plus task supervision.
 *
 * The hardware watchdog only proves that the main loop runs. Other tasks can
 * hang while the main loop keeps running: the power key task, the link task,
 * a command worker. So each such task registers here and checks in. The main
 * loop feeds the hardware only while every registered task has checked in
 * within its own timeout. When one is late, the main loop records which task
 * stalled (the next boot logs it) and stops feeding, and the chip resets.
 *
 * A 5 s request rounds up to 8.192 s, then a ~0.5 s reset stage resets the
 * chip. The boot's legacy CONFIG_WATCHDOG_ENABLE stays off: its ISR requests
 * recovery mode.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "lisa_device.h"
#include "lisa_wdt.h"

#include "platform_arcs.h"

#define TAG "plat_wdt"
#include "lisa_log.h"

#define WDT_DEVICE      "wdt0"
#define WDT_INT_MS      5000U
#define WDT_RST_MS      500U
#define MAX_TASKS       8

typedef struct {
    const char *name;
    uint32_t timeout_ms;
    volatile uint32_t last_ms;
    const char *volatile where;
    volatile bool paused;
} watched_t;

static lisa_device_t *s_wdt;
static bool s_running;
static bool s_stalled;
static watched_t s_tasks[MAX_TASKS];
static int s_count;

static uint32_t now_ms(void)
{
    return (uint32_t)plat_uptime_ms();
}

static void expired(void *user)
{
    (void)user;   /* the hardware reset stage finishes the job */
}

int plat_watchdog_register(const char *name, uint32_t timeout_ms)
{
    int slot = -1;
    taskENTER_CRITICAL();
    if (s_count < MAX_TASKS) {
        slot = s_count;
        s_tasks[slot].name = name;
        s_tasks[slot].timeout_ms = timeout_ms;
        s_tasks[slot].last_ms = now_ms();
        s_count++;
    }
    taskEXIT_CRITICAL();
    if (slot < 0) {
        LISA_LOGE(TAG, "no slot for %s", name);
    }
    return slot;
}

void plat_watchdog_checkin(int slot)
{
    if (slot >= 0 && slot < s_count) {
        s_tasks[slot].last_ms = now_ms();
    }
}

void plat_watchdog_at(int slot, const char *where)
{
    if (slot >= 0 && slot < s_count) {
        s_tasks[slot].where = where;
        s_tasks[slot].last_ms = now_ms();
    }
}

void plat_watchdog_pause(int slot, bool paused)
{
    if (slot >= 0 && slot < s_count) {
        s_tasks[slot].last_ms = now_ms();
        s_tasks[slot].paused = paused;
    }
}

int plat_watchdog_start(void)
{
    s_wdt = lisa_device_get(WDT_DEVICE);
    if (!lisa_device_ready(s_wdt)) {
        LISA_LOGE(TAG, "%s not ready", WDT_DEVICE);
        return -ENODEV;
    }
    lisa_wdt_config_t cfg = { .int_timeout_ms = WDT_INT_MS, .rst_timeout_ms = WDT_RST_MS };
    int r = lisa_wdt_setup(s_wdt, &cfg);
    if (r == LISA_DEVICE_OK) {
        r = lisa_wdt_set_callback(s_wdt, expired, NULL);
    }
    if (r == LISA_DEVICE_OK) {
        r = lisa_wdt_start(s_wdt);
    }
    if (r != LISA_DEVICE_OK) {
        LISA_LOGE(TAG, "start failed: %d", r);
        return r;
    }
    s_running = true;
    lisa_wdt_feed(s_wdt);
    LISA_LOGI(TAG, "armed: about 9 s without a feed resets the chip; %d tasks supervised", s_count);
    return 0;
}

void plat_boot_stage(const char *stage)
{
    plat_crash_boot_stage(stage);
    if (s_running) {
        lisa_wdt_feed(s_wdt);
    }
}

void plat_boot_done(void)
{
    plat_crash_boot_done();
}

void plat_watchdog_feed(void)
{
    if (!s_running || s_stalled) {
        return;
    }
    uint32_t now = now_ms();
    for (int i = 0; i < s_count; i++) {
        if (s_tasks[i].paused) {
            continue;
        }
        /* Signed: a task may check in between reading `now` and here (last_ms > now). */
        int32_t late = (int32_t)(now - s_tasks[i].last_ms);
        if (late > (int32_t)s_tasks[i].timeout_ms) {
            s_stalled = true;
            const char *where = s_tasks[i].where ? s_tasks[i].where : "?";
            plat_crash_note_stall(s_tasks[i].name, where, late);
            LISA_LOGE(TAG, "task %s silent for %u ms at %s: no more feeds, the chip resets", s_tasks[i].name,
                      (unsigned)late, where);
            return;
        }
    }
    lisa_wdt_feed(s_wdt);
}
