/*
 * The dog's 5-pixel strip (its eyes are pixels 2 and 4): a 50 ms task draws
 * either the SDK status (show_status) or an effect the agent chose with
 * chassis.set_light, which holds until display.show_animation (or "auto").
 * Frames come from dog_light_frame (dog_logic.c).
 */
#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "platform_arcs.h"
#include "gadget_board.h"
#include "dog_actuators.h"
#include "dog_board.h"
#include "dog_logic.h"

#define TAG "dog_light"
#include "lisa_log.h"

#define FRAME_MS        50
#define MAX_BRIGHTNESS  50     /* the LEDs are glaring at full power */

typedef struct {
    dog_light_effect_t effect;
    dog_rgb_t color;
    int brightness;            /* 0..100 of MAX_BRIGHTNESS */
} look_t;

static volatile bool s_manual;
static look_t s_manual_look;
static look_t s_status_look = { DOG_LIGHT_BREATHE_SLOW, { 255, 255, 255 }, 40 };
static volatile uint32_t s_look_since;

/* How the SDK's states look on the dog. */
static look_t status_look(gadget_status_t st)
{
    switch (st) {
    case GADGET_STATUS_BOOTING: return (look_t){ DOG_LIGHT_BREATHE_FAST, { 255, 255, 255 }, 40 };
    case GADGET_STATUS_UNPAIRED: return (look_t){ DOG_LIGHT_BLINK, { 0, 80, 255 }, 60 };
    case GADGET_STATUS_CONNECTING: return (look_t){ DOG_LIGHT_BREATHE_FAST, { 0, 80, 255 }, 60 };
    case GADGET_STATUS_LISTENING: return (look_t){ DOG_LIGHT_ON, { 0, 255, 60 }, 100 };
    case GADGET_STATUS_THINKING: return (look_t){ DOG_LIGHT_FLOWING, { 160, 0, 255 }, 80 };
    case GADGET_STATUS_SPEAKING: return (look_t){ DOG_LIGHT_BREATHE_FAST, { 0, 220, 255 }, 80 };
    case GADGET_STATUS_OFFLINE: return (look_t){ DOG_LIGHT_BREATHE_SLOW, { 255, 30, 0 }, 50 };
    case GADGET_STATUS_LOW_BATTERY: return (look_t){ DOG_LIGHT_BLINK, { 255, 0, 0 }, 60 };
    case GADGET_STATUS_CHARGING: return (look_t){ DOG_LIGHT_BREATHE_SLOW, { 255, 120, 0 }, 40 };
    case GADGET_STATUS_UPDATING: return (look_t){ DOG_LIGHT_FLOWING, { 255, 255, 255 }, 60 };
    case GADGET_STATUS_IDLE:
    default: return (look_t){ DOG_LIGHT_ON, { 255, 180, 80 }, 15 };   /* calm warm eyes */
    }
}

void dog_show_status(gadget_status_t st)
{
    s_status_look = status_look(st);
    s_look_since = (uint32_t)plat_uptime_ms();
}

void dog_show_animation(void)
{
    s_manual = false;
    s_look_since = (uint32_t)plat_uptime_ms();
}

int dog_light_set(const char *effect, uint8_t r, uint8_t g, uint8_t b, int brightness)
{
    if (strcmp(effect, "auto") == 0) {
        dog_show_animation();
        return 0;
    }
    look_t l = { DOG_LIGHT_OFF, { r, g, b }, brightness };
    if (!dog_light_effect_from_name(effect, &l.effect)) {
        return -1;
    }
    s_manual_look = l;
    s_manual = true;
    s_look_since = (uint32_t)plat_uptime_ms();
    return 0;
}

bool dog_light_manual(void)
{
    return s_manual;
}

static void light_task(void *arg)
{
    (void)arg;
    dog_rgb_t px[DOG_PIXELS], last[DOG_PIXELS];
    memset(last, 0xff, sizeof(last));
    for (;;) {
        look_t l = s_manual ? s_manual_look : s_status_look;
        uint32_t t = (uint32_t)plat_uptime_ms() - s_look_since;
        dog_light_frame(l.effect, l.color, l.brightness * MAX_BRIGHTNESS / 100, t, px);
        if (memcmp(px, last, sizeof(px)) != 0) {
            service_v2_rgb_color_t frame[DOG_PIXELS];
            for (int i = 0; i < DOG_PIXELS; i++) {
                frame[i] = (service_v2_rgb_color_t){ .red = px[i].r, .green = px[i].g, .blue = px[i].b };
            }
            service_v2_actuators_rgb_system_frame(frame);
            memcpy(last, px, sizeof(px));
        }
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}

int dog_light_start(void)
{
    s_look_since = (uint32_t)plat_uptime_ms();
    return xTaskCreate(light_task, "dog_light", 2048 / sizeof(StackType_t), NULL, 2, NULL) == pdPASS ? 0 : -1;
}
