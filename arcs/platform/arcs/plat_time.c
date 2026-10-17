/*
 * Wall clock: SNTP sets a UTC offset over the monotonic tick counter.
 * No RTC is used; the clock is valid only after the first sync in this boot.
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "lisa_sntp.h"
#include "lisa_time.h"
#include "shell.h"

#include "platform_arcs.h"

#define TAG "plat_time"
#include "lisa_log.h"

#define RETRY_MS         5000
#define RESYNC_MS        (6u * 3600u * 1000u)
#define QUERY_TIMEOUT_MS 5000

static const char *s_servers[] = {
    "time.cloudflare.com",
    "pool.ntp.org",
    "ntp.aliyun.com",
    "time.google.com",
};

static volatile int64_t s_offset_ms;   /* UTC ms = tick ms + offset */
static volatile bool s_synced;

static int sync_once(void)
{
    struct lisa_sntp_time t = {0};
    int ret = lisa_sntp_query(s_servers, sizeof(s_servers) / sizeof(s_servers[0]), QUERY_TIMEOUT_MS, &t);
    if (ret != 0) {
        LISA_LOGW(TAG, "sntp failed: %d", ret);
        return ret;
    }
    int64_t utc_ms = (int64_t)t.sec * 1000 + t.nsec / 1000000;
    s_offset_ms = utc_ms - (int64_t)lisa_os_get_tick_ms();
    s_synced = true;
    LISA_LOGI(TAG, "synced utc=%llu", (unsigned long long)t.sec);
    return 0;
}

static void time_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!plat_wifi_has_ip()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(sync_once() == 0 ? RESYNC_MS : RETRY_MS));
    }
}

int plat_time_start(void)
{
    return xTaskCreate(time_task, "plat_time", 2048, NULL, 4, NULL) == pdPASS ? 0 : -1;
}

bool plat_time_synced(void)
{
    return s_synced;
}

time_t plat_time_utc(void)
{
    if (!s_synced) {
        return (time_t)-1;
    }
    return (time_t)(((int64_t)lisa_os_get_tick_ms() + s_offset_ms) / 1000);
}

bool plat_time_wait(uint32_t timeout_ms)
{
    uint64_t deadline = lisa_os_get_tick_ms() + timeout_ms;
    while (!s_synced && lisa_os_get_tick_ms() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return s_synced;
}

/* mbedtls hooks (see gadget_mbedtls_config.h). */
time_t plat_tls_time(time_t *out)
{
    time_t now = plat_time_utc();
    if (out != NULL) {
        *out = now;
    }
    return now;
}

struct tm *mbedtls_platform_gmtime_r(const time_t *tt, struct tm *tm_buf)
{
    if (tt == NULL || tm_buf == NULL || *tt < 0) {
        return NULL;
    }
    return gmtime_r(tt, tm_buf);
}

static int time_cmd(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "sync") == 0) {
        shellPrint(shellGetCurrent(), "sync: %d\n", sync_once());
    }
    long long now = (long long)plat_time_utc();
    shellPrint(shellGetCurrent(), "synced=%d utc=%lld\n", s_synced, now);
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 time, time_cmd, time [sync]);
