/* L3: BLE pairing with the Impo iOS app. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Start BLE and the setup worker. Call after plat_init(). */
int pairing_service_start(void);

#ifdef __cplusplus
}
#endif
