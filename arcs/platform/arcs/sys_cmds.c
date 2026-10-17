/* Shell commands every gadget build keeps: reboot, recovery (used by adb_download.sh), poweroff, version, wdt_test. */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "shell.h"
#include "power_manager.h"
#include "uboot_recovery_api.h"
#include "lisa_kv.h"

#define TAG "sys_cmds"
#include "lisa_log.h"

#define KV_BOOT_HANG "debug.boot_hang"

#include "platform_arcs.h"

static int reboot_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    power_reboot_soft();
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 reboot, reboot_cmd, reboot);

static int recovery_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    int ret = uboot_recovery_request(UBOOT_RECOVERY_MODE_SOFT);
    if (ret != 0) {
        shellPrint(shellGetCurrent(), "request recovery failed: %d\n", ret);
        return ret;
    }
    power_reboot_soft();
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 recovery, recovery_cmd, reboot into adb recovery);

static int poweroff_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    power_shutdown();
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 poweroff, poweroff_cmd, power off);

static int version_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    shellPrint(shellGetCurrent(), "impo-gadget-arcs %s\n", plat_firmware_version());
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 version, version_cmd, show firmware version);

/* Watchdog self-test. "task": a supervised task goes silent, the main loop stops feeding.
 * "hang": the whole CPU spins with interrupts off, nothing feeds. Both must reset the chip. */
static int wdt_test_cmd(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "task") == 0) {
        plat_watchdog_at(plat_watchdog_register("wdt_test", 1000), "wdt_test");
        shellPrint(shellGetCurrent(), "fake task registered, never checks in: reset expected within ~10 s\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "hang") == 0) {
        shellPrint(shellGetCurrent(), "spinning with interrupts off: reset expected within ~9 s\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        taskDISABLE_INTERRUPTS();
        for (;;) {
        }
    }
    if (argc == 2 && strcmp(argv[1], "boot") == 0) {
        lisa_kv_set_int(KV_BOOT_HANG, 1);
        shellPrint(shellGetCurrent(), "the next boot hangs once in gadget_start: reset expected, then a \"hung in\" line\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        power_reboot_soft();
        return 0;
    }
    shellPrint(shellGetCurrent(), "usage: wdt_test task|hang|boot\n");
    return -1;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 wdt_test, wdt_test_cmd, watchdog self-test: task|hang|boot);

/* "wdt_test boot": hang once during boot, the way a stuck driver would. The flag is
 * cleared first, so the boot after the reset runs normally. */
void plat_boot_hang_test(void)
{
    int v = 0;
    if (lisa_kv_get_int(KV_BOOT_HANG, &v) != 0 || v != 1) {
        return;
    }
    lisa_kv_set_int(KV_BOOT_HANG, 0);
    LISA_LOGW(TAG, "boot hang test: spinning with interrupts off");
    vTaskDelay(pdMS_TO_TICKS(100));
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}
