/*
 * Robot dog battery: PB05 on adc0 channel 3 through a 2/5 divider, 10-bit,
 * 3.6 V reference; TP4056 CHRG on PB08, low while charging; USB detect via
 * the power manager. Curves measured on the 240032 cell (mtt1.0-impo battery.c).
 */
#include <stdbool.h>
#include <stdint.h>

#include "lisa_adc.h"
#include "lisa_gpio.h"
#include "power_manager.h"
#include "pinmux.h"

#include "dog_board.h"
#include "dog_motion.h"

#define BAT_ADC_CH        3
#define DIVIDER_PCT       40      /* 2/5 */
#define NO_BATTERY_MV     2000
#define SAMPLES           5

static const uint16_t s_discharge[11] = { 3010, 3440, 3576, 3719, 3781, 3820, 3860, 3925, 3973, 4013, 4160 };
static const uint16_t s_charge[11] = { 3257, 3677, 3757, 3850, 3943, 4002, 4039, 4071, 4103, 4150, 4200 };

static lisa_device_t *s_adc;
static lisa_device_t *s_chg;
static bool s_ready;

static int pct_from(uint16_t mv, const uint16_t *t)
{
    if (mv <= t[0]) {
        return 0;
    }
    if (mv >= t[10]) {
        return 100;
    }
    for (int i = 0; i < 10; i++) {
        if (mv <= t[i + 1]) {
            return i * 10 + (int)((mv - t[i]) * 10 / (t[i + 1] - t[i]));
        }
    }
    return 100;
}

int dog_power_init(void)
{
    s_adc = lisa_device_get("adc0");
    lisa_adc_channel_config_t cfg = { .reference = LISA_ADC_REF_VDD_3V6, .resolution = LISA_ADC_RESOLUTION_10BIT };
    s_ready = lisa_device_ready(s_adc) && lisa_adc_channel_setup(s_adc, BAT_ADC_CH, &cfg) == LISA_DEVICE_OK;
    s_chg = lisa_device_get(CHARGE_DET_DEVICE_NAME);
    if (lisa_device_ready(s_chg)) {
        lisa_gpio_configure(s_chg, CHARGE_DET_PIN, LISA_GPIO_CONFIG_INPUT_PULLUP);
    }
    return s_ready ? 0 : -1;
}

int dog_read_power(gadget_power_t *out)
{
    *out = (gadget_power_t){0};
    out->usb_power = power_is_usb_plugged();
    if (lisa_device_ready(s_chg)) {
        out->charging = lisa_gpio_read_pin(s_chg, CHARGE_DET_PIN) == CHARGE_DET_ACTIVE_LEVEL;
    }
    if (!s_ready) {
        return -1;
    }
    uint32_t sum = 0;
    int n = 0;
    for (int i = 0; i < SAMPLES; i++) {
        uint16_t raw = LISA_ADC_VALUE_INVALID;
        if (lisa_adc_read(s_adc, BAT_ADC_CH, &raw) == LISA_DEVICE_OK && raw <= 1023U) {
            sum += LISA_ADC_RAW_TO_MV(raw, 3600, LISA_ADC_RESOLUTION_10BIT);
            n++;
        }
    }
    if (n == 0) {
        return -1;
    }
    uint16_t mv = (uint16_t)(sum / n * 100U / DIVIDER_PCT);
    out->has_battery = mv >= NO_BATTERY_MV;
    out->battery_mv = mv;
    bool external = out->usb_power || out->charging;
    out->battery_pct = out->has_battery ? pct_from(mv, external ? s_charge : s_discharge) : 0;
    return 0;
}

int dog_power_off(void)
{
    power_shutdown();
    return -1;
}

/* The two TTP223 pads on its back (PA09, PA13) act as one AUX button: down while
 * either is touched, debounced over 3 polls. */
static unsigned poll_touch(void)
{
    static bool stable, last;
    static uint8_t same;
    bool a = false, b = false;
    dog_motion_touch_read(0, &a);
    dog_motion_touch_read(1, &b);
    bool now = a || b;
    same = (now == last) ? (uint8_t)(same + 1) : 0;
    last = now;
    if (same >= 3 && now != stable) {
        stable = now;
        return stable ? GADGET_BTN_AUX_PRESS : GADGET_BTN_AUX_RELEASE;
    }
    return 0;
}

/* Power key on PB04, active low, debounced over 3 polls (30 ms). The power
 * manager already configured the pad as an input with pull-up. */
static unsigned poll_key(void)
{
    static lisa_device_t *key;
    static bool stable, last;
    static uint8_t same;
    if (key == NULL) {
        key = lisa_device_get(POWER_KEY_DEVICE_NAME);
        if (!lisa_device_ready(key)) {
            key = NULL;
            return 0;
        }
        stable = last = lisa_gpio_read_pin(key, POWER_KEY_PIN) == LISA_GPIO_LOW;
        return stable ? GADGET_BTN_MAIN_PRESS : 0;
    }
    bool now = lisa_gpio_read_pin(key, POWER_KEY_PIN) == LISA_GPIO_LOW;
    same = (now == last) ? (uint8_t)(same + 1) : 0;
    last = now;
    if (same >= 3 && now != stable) {
        stable = now;
        return stable ? GADGET_BTN_MAIN_PRESS : GADGET_BTN_MAIN_RELEASE;
    }
    return 0;
}

unsigned dog_poll_buttons(void)
{
    return poll_key() | poll_touch();
}

/* Debug: raw levels of both touch pads for 15 s, printed on every change. */
#include "shell.h"
#include "FreeRTOS.h"
#include "task.h"

static int touch_raw_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    lisa_device_t *g = lisa_device_get("gpioa");
    int last9 = -2, last13 = -2;
    for (int i = 0; i < 300; i++) {
        int a = lisa_gpio_read_pin(g, 9);
        int b = lisa_gpio_read_pin(g, 13);
        if (a != last9 || b != last13) {
            shellPrint(shellGetCurrent(), "t=%d ms PA09=%d PA13=%d\n", i * 50, a, b);
            last9 = a;
            last13 = b;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    shellPrint(shellGetCurrent(), "touch_raw done\n");
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 touch_raw, touch_raw_cmd, debug: raw touch pad levels for 15 s);
