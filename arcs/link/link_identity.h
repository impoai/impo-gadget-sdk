/* Device identity shared by the link (link.register) and BLE setup (device_info, transcript). */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

const char *gadget_mac_str(void);    /* "aa:bb:cc:dd:ee:ff" (Wi-Fi MAC) */
const char *gadget_node_id(void);    /* "<prefix>-ddeeff" */
const char *gadget_device_id(void);  /* "hatch-link:<mac>" */
const char *gadget_ble_name(void);   /* "ImpoGadget-DDEEFF" */

#ifdef __cplusplus
}
#endif
