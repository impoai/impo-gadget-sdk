/*
 * What noise_control.cpp (esp32/main) asks of the gadget while it builds
 * link.register and answers device.health (impo_commands.h, impo_state.h of
 * upstream esp32/components/impo), answered from gadget/.
 */
#include <string.h>

#include "cJSON.h"

#include "gadget_board.h"
#include "gadget_core.h"
#include "gadget_status.h"
#include "impo_commands.h"
#include "impo_state.h"

#define TAG "link"
#include "lisa_log.h"

static const char *text(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "?";
}

/* After upstream's own device.set_name and device.health: the gadget's other commands. */
void impo_commands_describe(cJSON *commands)
{
    gadget_describe_commands(commands);
}

/*
 * Runs while link.register's params are built, after upstream has set its
 * ESP32 identity. Besides the capabilities, it puts ARCS's in place, the way
 * upstream build_register_json does for the X4's model_id
 * (cJSON_ReplaceItemInObject): platform and device_family "arcs" ("link"
 * would get the ESP32 Link firmware pushed as an OTA) and the board's model_id.
 */
void impo_commands_capabilities(cJSON *params)
{
    cJSON_ReplaceItemInObject(params, "platform", cJSON_CreateString("arcs"));
    cJSON_ReplaceItemInObject(params, "device_family", cJSON_CreateString("arcs"));
    cJSON_ReplaceItemInObject(params, "model_id", cJSON_CreateString(gadget_board_get()->model_id));
    gadget_fill_capabilities(params);

    const cJSON *product = cJSON_GetObjectItem(params, "product_id");
    const cJSON *unit = cJSON_GetObjectItem(params, "unit_id");
    LISA_LOGI(TAG, "link.register: node_id=%s display_name=\"%s\" platform=%s device_family=%s model_id=%s version=%s "
                   "unit_id=%s product_id=%s",
              text(params, "node_id"), text(params, "display_name"), text(params, "platform"),
              text(params, "device_family"), text(params, "model_id"), text(params, "version"),
              cJSON_IsString(unit) ? unit->valuestring : "(not attested)",
              cJSON_IsString(product) ? product->valuestring : "(none)");
}

impo_power_t impo_state_power(void)
{
    impo_power_t out = { .battery_pct = -1, .battery_mv = 0, .charging = false, .usb = false };
    gadget_power_t p;
    const gadget_board_t *b = gadget_board_get();
    if (b->read_power == NULL || !(gadget_status_power(&p) || b->read_power(&p) == 0)) {
        return out;
    }
    if (p.has_battery) {
        out.battery_pct = p.battery_pct;
        out.battery_mv = p.battery_mv;
        out.charging = p.charging;
    }
    out.usb = p.usb_power;
    return out;
}
