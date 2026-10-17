// SPDX-License-Identifier: MIT
#pragma once

// The classic ESP32 is the one target without an HMAC peripheral.
#ifndef FAKE_TARGET_ESP32
#define SOC_HMAC_SUPPORTED 1
#endif
