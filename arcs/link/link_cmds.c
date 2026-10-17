/*
 * Shell commands for bring-up of the link layer.
 *   https <url> [bearer]   GET a URL over verified TLS and print status and body.
 * Network work runs on its own task: a TLS handshake needs more stack than the shell has.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "shell.h"

#include "platform_arcs.h"
#include "http_client.h"
#include "impo_link.h"

#define TAG "link_cmd"
#include "lisa_log.h"

#define WORKER_STACK_WORDS (12 * 1024 / sizeof(StackType_t))
#define MAX_RESPONSE       4096

typedef struct {
    char url[512];
    char headers[600];
} https_job_t;

static void https_task(void *arg)
{
    https_job_t *job = arg;
    http_response_t resp;
    uint64_t t0 = xTaskGetTickCount();
    int ret = http_request("GET", job->url, job->headers, NULL, 0, MAX_RESPONSE, &resp);
    unsigned ms = (unsigned)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
    if (ret != 0) {
        LISA_LOGE(TAG, "https: failed %d (-0x%04x) after %u ms", ret, ret < 0 ? -ret : ret, ms);
    } else {
        LISA_LOGI(TAG, "https: status %d, %u bytes, %u ms: %.*s", resp.status, (unsigned)resp.body_len, ms,
               (int)(resp.body_len > 1024 ? 1024 : resp.body_len), resp.body);
        http_response_free(&resp);
    }
    LISA_LOGI(TAG, "https: stack headroom %u bytes", (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    free(job);
    vTaskDelete(NULL);
}

static int https_cmd(int argc, char **argv)
{
    if (argc < 2) {
        shellPrint(shellGetCurrent(), "usage: https <url> [bearer]\n");
        return -1;
    }
    https_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        return -1;
    }
    snprintf(job->url, sizeof(job->url), "%s", argv[1]);
    int n = snprintf(job->headers, sizeof(job->headers), "Accept: application/json\r\nX-API-Version: 1.0.0\r\n");
    if (argc >= 3) {
        snprintf(job->headers + n, sizeof(job->headers) - n, "Authorization: Bearer %s\r\n", argv[2]);
    }
    if (xTaskCreate(https_task, "https", WORKER_STACK_WORDS, job, 4, NULL) != pdPASS) {
        free(job);
        shellPrint(shellGetCurrent(), "https: task create failed\n");
        return -1;
    }
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 https, https_cmd, https <url> [bearer]);

static int link_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    size_t cap = 2048;
    char *text = malloc(cap);
    if (text == NULL) {
        return -1;
    }
    impo_link_describe(text, cap);
    /* One line at a time: the shell's print buffer is small. */
    for (char *line = text, *nl; line != NULL && *line != '\0'; line = nl ? nl + 1 : NULL) {
        nl = strchr(line, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        shellPrint(shellGetCurrent(), "%s\n", line);
    }
    free(text);
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 link, link_cmd, link status);
