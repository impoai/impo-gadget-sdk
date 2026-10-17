/*
 * Platform bring-up, in the order the AP image expects: IPC first, then the
 * services that depend on it. Taken from mtt1.0-impo's voice_platform_init
 * with every product feature removed.
 */
#include "sys_init.h"
#include "cJSON.h"
#include "ipc_master.h"
#include "lisa_kv.h"
#include "lisa_mem.h"
#include "netdev.h"
#include "sysheap.h"
#include "chip.h"
#include "spiflash.h"
#include "nvs.h"

#include "platform_arcs.h"
#include "lisa_time.h"

#ifndef GADGET_FW_VERSION
#define GADGET_FW_VERSION "0.1.0"
#endif

#define TAG "plat"
#include "lisa_log.h"

extern int lisa_shell_init(void);
extern int ic_message_init();
extern int sal_init(void);
extern void ipc_mem_init(int32_t wifi_ipc_mode);
extern int32_t ic_lock_init(void);

#if CONFIG_LISA_FLASH_ARCS_HALT_REMOTE_CORE && CONFIG_WIFI_LWIP_SAME_CORE
static int plat_ipc_early_init(void)
{
    ic_lock_init();
    ipc_master_init(NULL);
    return 0;
}
SYS_INIT(plat_ipc_early_init, SYS_INIT_LEVEL_PRE_DEVICES_INIT, 5);
#endif

#if CFG_NVS
/* Wi-Fi/BT calibration data lives in the last 32 KB of flash (matches mtt1.0-impo). */
#define NVDS_FLASH_ADDRESS_OFFSET 0xFF8000
#define NVDS_FLASH_SIZE           0x8000
static struct nvs_fs s_nvs_fs;
static FLASH_DEV s_flash_dev = {
    .base_addr = CMN_FLASHC_BASE, .d_width = 4, .sclk_div = 0xFF,
    .run_mod = RUN_WITHOUT_INT, .timeout = 0x180000,
};

static void plat_nvs_init(void)
{
    struct flash_pages_info info;
    flash_if_init(&s_flash_dev, 0, 0);
    s_nvs_fs.offset = NVDS_FLASH_ADDRESS_OFFSET;
    s_nvs_fs.flash_device = &s_flash_dev;
    flash_get_page_info_by_offs(&s_flash_dev, s_nvs_fs.offset, &info);
    s_nvs_fs.sector_size = info.size;
    s_nvs_fs.sector_count = NVDS_FLASH_SIZE / info.size;
    nvds_init(&s_nvs_fs);
}
#else
static void plat_nvs_init(void) {}
#endif

static void *cjson_malloc(size_t size) { return lisa_mem_alloc(size); }
static void cjson_free(void *ptr) { lisa_mem_free(ptr); }

/* The FreeRTOS tick starts from the RTC calendar, not 0, so uptime needs a base. */
static uint64_t s_boot_ms;

uint64_t plat_uptime_ms(void)
{
    return lisa_os_get_tick_ms() - s_boot_ms;
}

int plat_init(void)
{
    s_boot_ms = lisa_os_get_tick_ms();
    struct ipc_master_cb_tag ipc_cb = {0};
    cJSON_Hooks hooks = { .malloc_fn = cjson_malloc, .free_fn = cjson_free };

    plat_crash_init();   /* reads the previous boot's record before this boot writes its own */
    /*
     * Armed before anything that can hang. A hang during boot used to leave the
     * dog frozen with PWR_LOCK held: no USB, no power key (software reads it),
     * only a flat battery ended it. Now the chip resets and USB recovery works.
     */
    plat_watchdog_start();
    heap_caps_malloc_extmem_enable(16);
    cJSON_InitHooks(&hooks);
    plat_boot_stage("ipc with the AP core");
    ipc_mem_init(1);
    int ipc_ready = ipc_master_init(&ipc_cb) == 0;

    plat_boot_stage("shell");
    lisa_shell_init();
    if (ipc_ready && ic_message_init() != 0) {
        LISA_LOGE(TAG, "ic_message_init failed");
    }

    plat_boot_stage("kv");
    lisa_kv_init();
    plat_boot_stage("usb");
    plat_usb_start();
    plat_boot_stage("nvs");
    plat_nvs_init();

    plat_boot_stage("netdev");
    netdev_init();
    sal_init();
    plat_boot_stage("wifi start");

    if (!ipc_ready) {
        LISA_LOGE(TAG, "IPC with the AP core failed; Wi-Fi will not start");
        return -1;
    }
    if (plat_wifi_start() != 0) {
        return -1;
    }
    return plat_time_start();
}

const char *plat_firmware_version(void)
{
    return GADGET_FW_VERSION;   /* the bare version: device.health, BLE setup, User-Agent */
}
