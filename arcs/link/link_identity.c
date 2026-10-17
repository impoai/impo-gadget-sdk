/* Device identity derived once from the Wi-Fi MAC. */
#include <stdio.h>
#include <stdint.h>

#include "platform_arcs.h"
#include "link_identity.h"

static struct {
    int ready;
    char mac[18];
    char node_id[48];
    char device_id[32];
    char ble_name[32];
} s_id;

static void ensure(void)
{
    if (s_id.ready) {
        return;
    }
    uint8_t m[6] = {0};
    plat_wifi_get_mac(m);
    snprintf(s_id.mac, sizeof(s_id.mac), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    snprintf(s_id.node_id, sizeof(s_id.node_id), "%s-%02x%02x%02x", CONFIG_GADGET_NODE_PREFIX, m[3], m[4], m[5]);
    snprintf(s_id.device_id, sizeof(s_id.device_id), "hatch-link:%s", s_id.mac);
    snprintf(s_id.ble_name, sizeof(s_id.ble_name), "ImpoGadget-%02X%02X%02X", m[3], m[4], m[5]);
    s_id.ready = 1;
}

const char *gadget_mac_str(void) { ensure(); return s_id.mac; }
const char *gadget_node_id(void) { ensure(); return s_id.node_id; }
const char *gadget_device_id(void) { ensure(); return s_id.device_id; }
const char *gadget_ble_name(void) { ensure(); return s_id.ble_name; }
