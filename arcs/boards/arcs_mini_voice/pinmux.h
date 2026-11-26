/* SPDX-License-Identifier: MIT
 * Pin aliases match ListenAI arcs_mini's board, not the V2.1 robot dog.
 */
#pragma once
#define PA_EN_PIN 1
#define USB_DET_PIN 0
#define LED_PIN 1
#define POWER_EN_PIN 3
#define POWER_KEY_PIN 4
#define LCD_PWM_PIN 21
#define LCD_CS_PIN 22
#define LCD_CD_PIN 23
#define LCD_SPI_DATA_PIN 24
#define LCD_SPI_CLK_PIN 25
#define LCD_RST_PIN 9
void lisa_gpioa_pinmux(void);
void lisa_gpiob_pinmux(void);
void lisa_uart0_pinmux(void);
void lisa_pwm_pinmux(void);
void lisa_spi0_pinmux(void);
