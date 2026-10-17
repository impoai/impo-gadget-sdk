/* impo-gadget-arcs entry point. Brings the platform up, joins Wi-Fi and keeps the link to Impo online. */
#include "FreeRTOS.h"
#include "task.h"

#include "platform_arcs.h"
#include "impo_link.h"
#include "pairing_service.h"
#include "gadget_core.h"

#define TAG "main"
#include "lisa_log.h"

int main(void)
{
    LISA_LOGI(TAG, "impo-gadget-arcs %s starting", plat_firmware_version());
    if (plat_init() != 0) {
        LISA_LOGE(TAG, "platform init failed");
    } else {
        plat_boot_stage("gadget_start");   /* board, speaker, motion */
        plat_boot_hang_test();
        if (gadget_start() != 0) {
            LISA_LOGE(TAG, "board init failed");
        }
        plat_boot_stage("input");
        gadget_input_start();
        plat_boot_stage("link start");
        if (impo_link_start() != 0) {
            LISA_LOGE(TAG, "link start failed");
        }
        plat_boot_stage("pairing service");
        if (pairing_service_start() != 0) {
            LISA_LOGE(TAG, "pairing service start failed");
        }
    }
    /* The watchdog was armed in plat_init(); from here only this loop feeds it. */
    plat_boot_done();
    LISA_LOGI(TAG, "boot done in %u ms", (unsigned)plat_uptime_ms());

    bool had_ip = false;
    while (1) {
        plat_watchdog_feed();
        bool has_ip = plat_wifi_has_ip();
        if (has_ip != had_ip) {
            LISA_LOGI(TAG, "network %s", has_ip ? "up" : "down");
            if (has_ip) {
                plat_wifi_power_save(false);
            }
            had_ip = has_ip;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return 0;
}
