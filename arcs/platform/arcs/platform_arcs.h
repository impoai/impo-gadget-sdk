/* ARCS platform layer (L0): what the gadget layers above may use. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up IPC with the AP core, shell, KV, USB/ADB and sockets. Call once from main(). */
int plat_init(void);

/* Report the crash record left by the previous boot, and arm the exception hook. */
void plat_crash_init(void);

int plat_usb_start(void);

int plat_wifi_start(void);
bool plat_wifi_has_ip(void);
int plat_wifi_get_mac(uint8_t mac[6]);
int plat_wifi_get_ssid(char *out, size_t cap);   /* -1 when not connected */
/* Radio power save. Off while online, as impo-gadget-sdk wifi_mgr.c (WIFI_PS_NONE): a dozing
 * station can lose its NAT/AP state and the link gets reset. Call from a task, not a callback. */
int plat_wifi_power_save(bool on);
/* Logs a move to another router (access point) of the network; call every few seconds. */
void plat_wifi_watch_router(void);

typedef struct {
    char ssid[33];
    int rssi;
    bool secure;
} plat_wifi_ap_t;
/* Blocking scan; returns unique visible networks. */
int plat_wifi_scan(plat_wifi_ap_t *aps, int max);
/* Join a network and wait for DHCP. Saves it on success. 0 when an IP is up. */
int plat_wifi_join(const char *ssid, const char *password, uint32_t timeout_ms);

const char *plat_firmware_version(void);
uint64_t plat_uptime_ms(void);   /* since plat_init() */

/* System watchdog: ~9 s without plat_watchdog_feed() resets the chip. Feed from the main loop only.
 * The feed is skipped once a registered task misses its check-in, so a hung task also resets the chip. */
int plat_watchdog_start(void);
void plat_watchdog_feed(void);
/* A task that must not hang registers once (from any task, before or after start) and then calls
 * plat_watchdog_checkin() at least every timeout_ms. Returns the slot, or -1 when full. */
int plat_watchdog_register(const char *name, uint32_t timeout_ms);
void plat_watchdog_checkin(int slot);
/* Checks in and notes where the task is now ("ws_recv"); a stall record names the last spot. */
void plat_watchdog_at(int slot, const char *where);
/* A paused slot is not watched (its task is gone for now, e.g. a link session that ended);
 * resuming counts as a check-in. */
void plat_watchdog_pause(int slot, bool paused);

/* Records a stalled task in the crash record, for the next boot to log. */
void plat_crash_note_stall(const char *task, const char *where, uint32_t silent_ms);
/* Boot steps: feeds the watchdog and notes the step, so a hang resets the chip and the next boot names it. */
void plat_boot_stage(const char *stage);
/* The boot finished: forget the step marker. */
void plat_boot_done(void);
void plat_crash_boot_stage(const char *stage);
void plat_crash_boot_done(void);
/* wdt_test boot: hangs this boot once when the test flag is set. */
void plat_boot_hang_test(void);

/* ---- Speaker: the on-chip DAC at 16 kHz mono 16-bit. The SDK's gadget_sound owns it. ---- */
int plat_audio_start(void);
int plat_audio_write(const int16_t *pcm, size_t samples);   /* blocks while the DMA buffers are full */
/* Microphone, 16 kHz mono 16-bit; cb runs on the audio driver's task, keep it short. */
typedef void (*plat_audio_mic_cb_t)(const int16_t *pcm, size_t samples);
int plat_audio_mic_start(plat_audio_mic_cb_t cb);
void plat_audio_mic_stop(void);
void plat_audio_mic_stereo(bool on);   /* debug: interleaved L/R (R carries the speaker reference) */

/* ---- Wall clock (SNTP). TLS certificate checks need it. ---- */
int plat_time_start(void);              /* background sync once an IP is up */
bool plat_time_synced(void);
time_t plat_time_utc(void);             /* UTC seconds, or -1 before the first sync */
bool plat_time_wait(uint32_t timeout_ms);

/* ---- Randomness from the hardware TRNG ---- */
int plat_random(void *buf, size_t len);

/* ---- TLS client over a TCP socket. The peer is verified against plat_ca_bundle_pem. ---- */
#define PLAT_TLS_TIMEOUT (-2)
typedef struct plat_tls plat_tls_t;
/* Returns NULL on failure; *err (optional) gets the mbedtls or socket error. */
plat_tls_t *plat_tls_connect(const char *host, uint16_t port, uint32_t timeout_ms, int *err);
int plat_tls_write(plat_tls_t *tls, const void *buf, size_t len);   /* writes all, returns len or <0 */
/* >0 bytes read, 0 peer closed, PLAT_TLS_TIMEOUT, <0 error. */
int plat_tls_read(plat_tls_t *tls, void *buf, size_t len, uint32_t timeout_ms);
void plat_tls_close(plat_tls_t *tls);
int plat_tls_fd(const plat_tls_t *tls);          /* the SAL socket */
size_t plat_tls_pending(plat_tls_t *tls);        /* decrypted bytes a read returns without the socket */

extern const char plat_ca_bundle_pem[];

/* ---- BLE peripheral: one custom 128-bit service, RX write + TX notify ---- */
typedef struct {
    void (*on_connect)(void);
    void (*on_disconnect)(void);
    void (*on_write)(const uint8_t *data, size_t len);   /* BT task context: keep it short */
} plat_ble_cbs_t;
int plat_ble_start(const char *name, const char *svc_uuid, const char *rx_uuid, const char *tx_uuid,
                   const plat_ble_cbs_t *cbs);
/* setup_flag is the manufacturer-data byte the Impo app reads (1 = already set up). */
void plat_ble_set_advertising(bool on, uint8_t setup_flag);
size_t plat_ble_max_notify(void);
int plat_ble_notify(const uint8_t *data, size_t len);   /* waits for the stack to send it */
void plat_ble_disconnect(void);

#ifdef __cplusplus
}
#endif
