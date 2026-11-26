/* SPDX-License-Identifier: MIT
 * An application adds commands without editing the board or cloud protocol. */
#include "impo_arcs_mini.h"
static cJSON *product_info(const cJSON *params)
{
    (void)params;
    const impo_arcs_mini_product_t *p = impo_arcs_mini_product();
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "sdk_version", IMPO_ARCS_MINI_SDK_VERSION);
    cJSON_AddStringToObject(result, "product", p->id);
    cJSON_AddBoolToObject(result, "wake_available", impo_arcs_mini_wake_available());
    return gadget_command_ok_payload(result);
}
static const gadget_command_t commands[] = {
    { "companion.info", "Return this companion's product and local wake availability.",
      NULL, NULL, product_info }
};
const impo_arcs_mini_application_t *impo_arcs_mini_application(void)
{
    static const impo_arcs_mini_application_t app = {
        .commands = commands, .command_count = sizeof(commands) / sizeof(commands[0])
    };
    return &app;
}
