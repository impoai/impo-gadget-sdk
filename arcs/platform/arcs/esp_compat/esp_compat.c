/*
 * ESP-IDF system calls the shared link (esp32/main) makes, on ARCS: randomness, the app
 * description, error names, and task supervision for the link's own tasks.
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "sysutils.h"

#include "platform_arcs.h"
#include "esp_compat.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_random.h"

uint32_t esp_random(void)
{
    uint32_t v = 0;
    plat_random(&v, sizeof(v));
    return v;
}

const esp_app_desc_t *esp_app_get_description(void)
{
    static esp_app_desc_t desc;
    if (desc.version[0] == '\0') {
        snprintf(desc.project_name, sizeof(desc.project_name), "impo-gadget-arcs");
        snprintf(desc.version, sizeof(desc.version), "%s", plat_firmware_version());
    }
    return &desc;
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
    default: return "ESP_ERR";
    }
}

esp_err_t esp_crt_bundle_attach(void *conf)
{
    (void)conf;
    return ESP_OK;
}

/* ---- product id (CONFIG_GADGET_PRODUCT_ID) ---- */

static __psram_bss__ char s_product_id[48];

void esp_compat_set_product_id(const char *id)
{
    snprintf(s_product_id, sizeof(s_product_id), "%s", id != NULL ? id : "");
}

const char *esp_compat_product_id(void)
{
    return s_product_id;
}

/* ---- supervision of the link's tasks ---- */

#define MAX_SUPERVISED 2

static struct {
    const char *name;
    int slot;
    TaskHandle_t handle;   /* the last task seen under that name */
} s_sup[MAX_SUPERVISED];
static int s_sup_n;

void esp_compat_wdt_supervise(const char *task_name, int slot)
{
    if (s_sup_n < MAX_SUPERVISED && slot >= 0) {
        s_sup[s_sup_n].name = task_name;
        s_sup[s_sup_n].slot = slot;
        s_sup_n++;
    }
}

void esp_compat_wdt_alive(const char *where)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < s_sup_n; i++) {
        if (s_sup[i].handle != me) {
            /* A new session task (noise_ctrl_connect starts one each time) under the same name. */
            if (strcmp(pcTaskGetName(me), s_sup[i].name) != 0) {
                continue;
            }
            s_sup[i].handle = me;
        }
        plat_watchdog_at(s_sup[i].slot, where);
        return;
    }
}

void esp_compat_task_delay(TickType_t ticks)
{
    esp_compat_wdt_alive("delay");
    vTaskDelay(ticks);
}
