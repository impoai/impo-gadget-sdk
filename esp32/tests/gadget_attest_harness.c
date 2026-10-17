// SPDX-License-Identifier: MIT
// Runs gadget_attest.c against fake eFuse, MAC and HMAC peripherals and
// prints each case's registration fields, which test_gadget_attest.py checks.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "gadget_attest.h"
#include "soc/soc_caps.h"

#if SOC_HMAC_SUPPORTED
#include "esp_efuse.h"
#include "esp_hmac.h"
#include "esp_mac.h"

static esp_efuse_purpose_t s_key5_purpose;
static bool s_hmac_fails;
static int s_hmac_key = -1;
static uint8_t s_message[128];
static size_t s_message_len;

esp_efuse_purpose_t esp_efuse_get_key_purpose(esp_efuse_block_t block) {
    return block == EFUSE_BLK_KEY5 ? s_key5_purpose : ESP_EFUSE_KEY_PURPOSE_USER;
}

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type) {
    if (type != ESP_MAC_EFUSE_FACTORY) return ESP_FAIL;
    static const uint8_t factory[6] = {0x70, 0xaf, 0x09, 0x6d, 0xc5, 0xa8};
    memcpy(mac, factory, sizeof(factory));
    return ESP_OK;
}

// Records the message and answers with bytes 0..31, so the test can check the
// exact message and the encoding without a real key.
esp_err_t esp_hmac_calculate(hmac_key_id_t key_id, const void *message,
                             size_t message_len, uint8_t *hmac) {
    s_hmac_key = key_id;
    s_message_len = message_len < sizeof(s_message) ? message_len : sizeof(s_message);
    memcpy(s_message, message, s_message_len);
    if (s_hmac_fails) return ESP_FAIL;
    for (int i = 0; i < 32; i++) hmac[i] = (uint8_t)i;
    return ESP_OK;
}
#endif

static void print_case(const char *name, const uint8_t *hash, size_t len) {
    cJSON *params = cJSON_CreateObject();
    gadget_attest_add(params, hash, len);
    cJSON *unit_id = cJSON_GetObjectItem(params, "unit_id");
    cJSON *attest = cJSON_GetObjectItem(params, "attest");
    printf("%s unit_id=%s attest=%s", name,
           unit_id ? unit_id->valuestring : "-", attest ? attest->valuestring : "-");
#if SOC_HMAC_SUPPORTED
    printf(" key=%d message=", s_hmac_key);
    for (size_t i = 0; i < s_message_len; i++) printf("%02x", s_message[i]);
    s_hmac_key = -1;
    s_message_len = 0;
#endif
    printf("\n");
    cJSON_Delete(params);
}

int main(void) {
    uint8_t hash[GADGET_ATTEST_HASH_SIZE];
    for (size_t i = 0; i < sizeof(hash); i++) hash[i] = (uint8_t)(0xa0 + i);

#if SOC_HMAC_SUPPORTED
    s_key5_purpose = ESP_EFUSE_KEY_PURPOSE_HMAC_UP;
    print_case("keyed", hash, sizeof(hash));
    print_case("no_session", NULL, 0);
    print_case("short_hash", hash, 16);
    s_hmac_fails = true;
    print_case("hmac_fails", hash, sizeof(hash));
    s_hmac_fails = false;
    s_key5_purpose = ESP_EFUSE_KEY_PURPOSE_USER;
    print_case("no_key", hash, sizeof(hash));
#else
    print_case("no_peripheral", hash, sizeof(hash));
#endif
    return 0;
}
