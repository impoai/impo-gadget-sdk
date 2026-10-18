# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Shared build-profile checks. Include after project().
set(GADGET_CONFIG_REGEN_HINT
    "Generated sdkconfig values override newer defaults; regenerate the active isolated profile configuration.")

# The X4 development port retains the vendor bootloader and USB recovery.
if(CONFIG_HOMEHUB_LED_BACKEND_XTEINK_X4 AND
   (CONFIG_SECURE_BOOT OR CONFIG_SECURE_FLASH_ENC_ENABLED OR
    CONFIG_HOMEHUB_PAIRING_EFUSE_AUTH OR CONFIG_HOMEHUB_NVS_ENCRYPTION OR
    CONFIG_NVS_ENCRYPTION OR CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT OR
    CONFIG_HOMEHUB_OTA_ENABLED))
    message(FATAL_ERROR "X4 requires the recoverable development security profile and USB updates.")
endif()

if(CONFIG_HOMEHUB_LED_BACKEND_XTEINK_X4 AND
   (NOT CONFIG_ESP_COREDUMP_ENABLE_TO_NONE OR
    (NOT CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS AND NOT CONFIG_SPI_FLASH_DANGEROUS_WRITE_FAILS)))
    message(FATAL_ERROR "X4 reserves the coredump data partition for its power-off image and requires flash write protection.")
endif()

if(NOT CONFIG_CJSON_NESTING_LIMIT EQUAL 16)
    message(FATAL_ERROR
        "ESP32 Device SDK requires cJSON nesting limit 16. ${GADGET_CONFIG_REGEN_HINT}")
endif()

# Old generated profiles retain 64 KiB TCP buffers and can exhaust the DMA heap
# during tunnel bursts even when sdkconfig.defaults has been updated.
# TCP limits: the no-PSRAM Cardputer, AI Passport and X4 have no tunnel and use
# four-MSS windows.
if((NOT CONFIG_LWIP_TCP_SND_BUF_DEFAULT EQUAL 16384 OR
    NOT CONFIG_LWIP_TCP_WND_DEFAULT EQUAL 16384) AND
   NOT ((CONFIG_IMPO_BOARD_M5STACK_CARDPUTER_ADV OR CONFIG_IMPO_BOARD_AI_PASSPORT OR CONFIG_HOMEHUB_LED_BACKEND_XTEINK_X4) AND NOT CONFIG_SPIRAM AND
        NOT CONFIG_HOMEHUB_TUNNEL AND CONFIG_LWIP_TCP_SND_BUF_DEFAULT EQUAL 5760 AND
        CONFIG_LWIP_TCP_WND_DEFAULT EQUAL 5760))
    message(FATAL_ERROR
        "ESP32 Device SDK requires TCP send buffer and receive window 16384 (5760 on the no-tunnel Cardputer ADV, AI Passport and X4) for DMA headroom. ${GADGET_CONFIG_REGEN_HINT}")
endif()

# A token from gadgets.impo.ai is mgst_ plus 43 canonical base64url characters.
# Manufacturer builds pair with fleet attestation instead.
if("${CONFIG_GADGET_SDK_TOKEN}" STREQUAL "")
    if(NOT CONFIG_HOMEHUB_PAIRING_EFUSE_AUTH)
        message(WARNING
            "No SDK token: set CONFIG_GADGET_SDK_TOKEN (idf.py menuconfig > ESP32 Device SDK) "
            "to the token from gadgets.impo.ai. Gadgets without one will stop pairing.")
    endif()
else()
    string(LENGTH "${CONFIG_GADGET_SDK_TOKEN}" GADGET_SDK_TOKEN_LENGTH)
    if(NOT GADGET_SDK_TOKEN_LENGTH EQUAL 48 OR
       NOT CONFIG_GADGET_SDK_TOKEN MATCHES "^mgst_[A-Za-z0-9_-]*[AEIMQUYcgkosw048]$")
        message(FATAL_ERROR
            "CONFIG_GADGET_SDK_TOKEN is not a valid SDK token. Copy it again from gadgets.impo.ai.")
    endif()
endif()

# A custom gadget's product ID, as the Impo developer platform issues it.
# Empty sends none.
# CMake regular expressions have no {n}, so the UUID pattern is spelled out.
string(REPEAT "[0-9a-f]" 4 GADGET_HEX4)
set(GADGET_UUID_PATTERN "^${GADGET_HEX4}${GADGET_HEX4}-${GADGET_HEX4}-${GADGET_HEX4}-${GADGET_HEX4}-${GADGET_HEX4}${GADGET_HEX4}${GADGET_HEX4}$")
if(NOT "${CONFIG_GADGET_PRODUCT_ID}" STREQUAL "" AND
   NOT "${CONFIG_GADGET_PRODUCT_ID}" MATCHES "${GADGET_UUID_PATTERN}")
    message(FATAL_ERROR
        "CONFIG_GADGET_PRODUCT_ID \"${CONFIG_GADGET_PRODUCT_ID}\" is not a product ID: "
        "a lowercase UUID. Copy it from the product on the Impo developer platform.")
endif()

# Manufacturer attestation must remain inaccessible to unsigned firmware.
# Community pairing is independent of logging, OTA signing, and board selection.
if(CONFIG_HOMEHUB_PAIRING_EFUSE_AUTH)
    if(NOT CONFIG_SECURE_BOOT)
        message(FATAL_ERROR
            "The hardware eFuse pairing key requires Secure Boot. ${GADGET_CONFIG_REGEN_HINT}")
    endif()
    if(NOT CONFIG_HOMEHUB_PAIRING_AUTH_EPOCH EQUAL 1)
        message(FATAL_ERROR
            "Manufacturer pairing builds must use production epoch 1. ${GADGET_CONFIG_REGEN_HINT}")
    endif()
endif()
