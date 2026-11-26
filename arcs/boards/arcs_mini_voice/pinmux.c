/* SPDX-License-Identifier: MIT */
#include "pinmux.h"
#include "IOMuxManager.h"
void lisa_gpioa_pinmux(void)
{
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, PA_EN_PIN, 1);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, LCD_CS_PIN, 0);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, LCD_CD_PIN, 0);
}
void lisa_gpiob_pinmux(void)
{
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_B, USB_DET_PIN, 0);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_B, LED_PIN, 0);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_B, POWER_EN_PIN, 0);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_B, POWER_KEY_PIN, 0);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_B, LCD_RST_PIN, 0);
}
void lisa_uart0_pinmux(void)
{
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, 2, 2);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, 3, 2);
}

/* The SDK may initialize adc0; this USB target routes no battery ADC. */
void lisa_adc_pinmux(void) {}
void lisa_pwm_pinmux(void)
{
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, LCD_PWM_PIN, 12);
}
void lisa_spi0_pinmux(void)
{
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, LCD_SPI_DATA_PIN, 5);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, LCD_SPI_CLK_PIN, 5);
}
