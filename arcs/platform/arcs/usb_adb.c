/*
 * USB device with the ADB shell. adb is how this firmware is flashed and
 * debugged, so it starts before anything that could fail.
 */
#include "FreeRTOS.h"
#include "task.h"
#include <stdbool.h>
#include "tusb.h"
#include "chip.h"
#include "adb.h"
#include "adb_shell.h"

#include "platform_arcs.h"

static void usb_task(void *arg)
{
    (void)arg;
    IP_SYSCTRL->REG_PERI_CLK_CFG6.bit.ENA_USB_CLK = 0x01;
    IP_CMN_SYS->REG_USB_CTRL1.bit.USBPHY_OUTCLKSEL = 0x1;
    IP_CMN_SYS->REG_USB_CTRL1.bit.USBC_CFG_IDDIG = 0x1;   /* B device */
    IP_CMN_SYS->REG_USB_CTRL1.bit.UTMI_DATABUS16_8 = 0x1; /* 16-bit UTMI */

    adb_init();
    adb_shell_init();

    tud_disconnect();
    tusb_init();
    tud_connect();

    while (1) {
        tud_task();
    }
}

int plat_usb_start(void)
{
    return xTaskCreate(usb_task, "usb_task", 1024, NULL, configMAX_PRIORITIES - 2, NULL) == pdPASS ? 0 : -1;
}

