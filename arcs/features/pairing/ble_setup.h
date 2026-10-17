/*
 * L3: Impo BLE setup protocol v5, device (peripheral) side, community mode.
 * Transport-independent: the BLE layer feeds written packets in and supplies
 * a notify function. Wire format matches impo-gadget-sdk (linux ble_setup.py,
 * esp32 ble_server.c) and the Impo iOS app (GadgetPairing.swift).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GATT layout the iOS app looks for. */
#define BLE_SETUP_SERVICE_UUID "7fdd3d1c-38ea-46cf-8b46-314ecf5f240c"
#define BLE_SETUP_RX_UUID      "4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01"  /* write */
#define BLE_SETUP_TX_UUID      "d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c"  /* notify */

#define BLE_SETUP_MAX_MESSAGE 8192

typedef struct {
    char ssid[33];
    int rssi;
    bool secure;
} ble_setup_ap_t;

typedef struct {
    /* Identity, also used in the transcript. */
    const char *node_id;
    const char *device_id;
    const char *mac;              /* "aa:bb:cc:dd:ee:ff" */
    const char *firmware_version;
    const char *policy;           /* "confirm_app" (no button) or "confirm_press" */

    /* Send one BLE notification packet (already framed). */
    int (*notify)(const uint8_t *data, size_t len);
    /* Largest notification payload: min(ATT_MTU - 3, 160). */
    size_t (*max_packet)(void);
    void (*disconnect)(void);
    void (*sleep_ms)(uint32_t ms);

    int (*wifi_scan)(ble_setup_ap_t *aps, int max);
    /* Blocking join; 0 when associated with an IP. */
    int (*wifi_connect)(const char *ssid, const char *password, uint32_t timeout_ms);
    int (*save_pairing)(const char *access, const char *refresh, const char *api_url, const char *noise_host);
    /* Provisioning finished; the BLE layer stops advertising and marks the device set up. */
    void (*on_setup_complete)(void);
} ble_setup_ops_t;

void ble_setup_init(const ble_setup_ops_t *ops);

/* Session lifecycle; call on the worker that runs ble_setup_handle. */
void ble_setup_on_connect(void);
void ble_setup_on_disconnect(void);
/* Already paired devices refuse a new hello until unpaired. */
void ble_setup_set_paired(bool paired);
/* Reassembly reset; call from the same context as ble_setup_reassemble. */
void ble_setup_rx_reset(void);

/* Feed one written packet. Returns a complete message (heap, NUL-terminated,
 * caller frees) when reassembly finishes, else NULL. Cheap; safe in BLE callbacks. */
char *ble_setup_reassemble(const uint8_t *data, size_t len, size_t *msg_len);

/* Handle one complete message. May block (Wi-Fi join); run it on a worker task. */
void ble_setup_handle(const char *msg, size_t len);

/* Physical confirmation for "confirm_press". */
void ble_setup_confirm_press(void);

/* Tests only: fix the device key and nonce used by the next hello. */
void ble_setup_set_test_keys(const uint8_t priv[32], const uint8_t nonce[16]);

#ifdef __cplusplus
}
#endif
