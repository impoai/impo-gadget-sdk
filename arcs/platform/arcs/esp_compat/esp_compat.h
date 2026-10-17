/*
 * ARCS side of the ESP-IDF compatibility layer: what the link glue (link/)
 * sets up and reads. The ESP-IDF headers themselves are in include/ and are
 * seen only by the esp32/main files built here.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CONFIG_GADGET_PRODUCT_ID for link.register; "" sends none. */
void esp_compat_set_product_id(const char *id);

/* Tasks named task_name check in with watchdog slot `slot` whenever they call
 * into the compat layer (vTaskDelay, TLS reads and writes, the session loop's
 * tunnel pump). The owner pauses the slot while no such task runs. */
void esp_compat_wdt_supervise(const char *task_name, int slot);
/* The check-in itself; `where` names the spot for a stall record. */
void esp_compat_wdt_alive(const char *where);

/* The newest connection made with esp_tls_conn_new_sync (the Noise session):
 * how long since it last received anything, and its last read error or end. */
uint32_t esp_compat_tls_rx_quiet_ms(void);
void esp_compat_tls_last_event(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
