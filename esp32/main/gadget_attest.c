// SPDX-License-Identifier: MIT
#include "gadget_attest.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "soc/soc_caps.h"

#if SOC_HMAC_SUPPORTED
#include "esp_efuse.h"
#include "esp_hmac.h"
#include "esp_mac.h"

static const char *TAG = "link.attest";

// Where the factory burns the unit key. Fixed by the contract, not by board.
#define ATTEST_KEY_BLOCK EFUSE_BLK_KEY5
#define ATTEST_HMAC_KEY  HMAC_KEY5

// The label and its terminating 0x00 byte, which the message includes.
static const char ATTEST_LABEL[] = "impo-gadget/attest/v1";

static void base64url(const uint8_t *in, size_t len, char *out) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = alphabet[(v >> 18) & 0x3f];
        out[o++] = alphabet[(v >> 12) & 0x3f];
        if (i + 1 < len) out[o++] = alphabet[(v >> 6) & 0x3f];
        if (i + 2 < len) out[o++] = alphabet[v & 0x3f];
    }
    out[o] = '\0';
}
#endif

bool gadget_attest_add(cJSON *params, const uint8_t *handshake_hash, size_t len) {
#if SOC_HMAC_SUPPORTED
    if (!params || !handshake_hash || len != GADGET_ATTEST_HASH_SIZE) return false;
    if (esp_efuse_get_key_purpose(ATTEST_KEY_BLOCK) != ESP_EFUSE_KEY_PURPOSE_HMAC_UP) {
        return false;
    }

    // A unit with a key that can't produce a proof sends neither field and
    // registers as custom: a made-up or partial attest would only be refused.
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) != ESP_OK) {
        ESP_LOGE(TAG, "factory MAC unreadable; registering without attestation");
        return false;
    }
    uint8_t msg[sizeof(ATTEST_LABEL) + GADGET_ATTEST_HASH_SIZE];
    memcpy(msg, ATTEST_LABEL, sizeof(ATTEST_LABEL));
    memcpy(msg + sizeof(ATTEST_LABEL), handshake_hash, GADGET_ATTEST_HASH_SIZE);
    uint8_t proof[32];
    esp_err_t err = esp_hmac_calculate(ATTEST_HMAC_KEY, msg, sizeof(msg), proof);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HMAC failed (%s); registering without attestation",
                 esp_err_to_name(err));
        return false;
    }

    char unit_id[13];
    snprintf(unit_id, sizeof(unit_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char attest[44];   // 32 bytes are 43 base64url characters
    base64url(proof, sizeof(proof), attest);
    cJSON_AddStringToObject(params, "unit_id", unit_id);
    cJSON_AddStringToObject(params, "attest", attest);
    return true;
#else
    (void)params;
    (void)handshake_hash;
    (void)len;
    return false;
#endif
}
