/*
 * What the shared link (esp32/main) calls into for features ARCS gadgets do not have:
 * the home-network tunnel, OTA, unit attestation. Each answers "not here" the
 * way upstream's own code does when the feature is off.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"

#include "esp_compat.h"
#include "gadget_attest.h"
#include "noise_tunnel_internal.h"
#include "ota.h"

/* ---- send headroom (adapted from upstream esp32/main/noise_tunnel.cpp, 8f07ace) ----
 * The same thresholds over the SDK's heap_caps. Every ARCS heap region is
 * DMA-capable (system/heap/sysheap.c), PSRAM included, so these hold unless
 * memory is really gone. */

#define NOISE_TX_DMA_RESERVE_BYTES    (16 * 1024)
#define NOISE_TX_DMA_CONTIGUOUS_BYTES (2 * 1024)

bool noise_tx_has_dma_headroom(size_t *dma_free)
{
    size_t available = heap_caps_get_free_size(MALLOC_CAP_DMA);
    if (dma_free) {
        *dma_free = available;
    }
    return available >= NOISE_TX_DMA_RESERVE_BYTES;
}

bool noise_tx_has_dma_headroom_reclaiming(size_t reclaimable)
{
    return heap_caps_get_free_size(MALLOC_CAP_DMA) + reclaimable >= NOISE_TX_DMA_RESERVE_BYTES;
}

bool noise_tx_has_contiguous_dma_headroom(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_DMA) >= NOISE_TX_DMA_CONTIGUOUS_BYTES;
}

/* ---- tunnel: none (as upstream with CONFIG_HOMEHUB_TUNNEL off, nothing is opened) ---- */

bool noise_tunnel_on_session_up(const noise_tunnel_emit_t *emit)
{
    (void)emit;
    return false;
}

void noise_tunnel_maybe_reopen(const noise_tunnel_emit_t *emit)
{
    (void)emit;
}

void noise_tunnel_on_session_down(void)
{
}

void noise_tunnel_on_inbound(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
}

/* The session loop calls this once every turn: the turn is its check-in. */
int noise_tunnel_pump_tx(const noise_tunnel_emit_t *emit)
{
    (void)emit;
    esp_compat_wdt_alive("session");
    return 0;
}

void noise_tunnel_tick(const noise_tunnel_emit_t *emit)
{
    (void)emit;
}

/* ---- OTA: not on ARCS yet (as upstream with CONFIG_HOMEHUB_OTA_ENABLED off) ---- */

bool ota_is_enabled(void)
{
    return false;
}

void ota_start(const char *url, bool force, ota_status_cb cb, void *user)
{
    (void)url;
    (void)force;
    if (cb != NULL) {
        ota_event_t ev = { OTA_RESULT_SKIPPED, "OTA is disabled in this build", NULL, NULL };
        cb(&ev, user);
    }
}

/* ---- attestation: ARCS has no eFuse unit key, so the gadget registers as custom
 * (as upstream esp32/main/gadget_attest.c on a chip without SOC_HMAC_SUPPORTED). ---- */

bool gadget_attest_add(cJSON *params, const uint8_t *handshake_hash, size_t len)
{
    (void)params;
    (void)handshake_hash;
    (void)len;
    return false;
}
