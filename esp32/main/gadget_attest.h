// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// The Noise handshake hash h is SHA-256 sized.
#define GADGET_ATTEST_HASH_SIZE 32

// Official gadgets prove which unit they are on every connection
// (contracts/gadget-identity.md in impoai/impo). When eFuse BLOCK_KEY5 holds a
// unit key (purpose HMAC_UP), this adds to link.register's params:
//
//   unit_id  the chip's factory MAC, 12 lowercase hex digits
//   attest   base64url, no padding, of
//            HMAC(unit key, "impo-gadget/attest/v1" 0x00 h)
//
// h is the current session's handshake hash, which binds the proof to this
// connection. Otherwise, and on chips without an HMAC peripheral (the classic
// ESP32), it adds neither field and the gadget registers as custom.
void gadget_attest_add(cJSON *params, const uint8_t *handshake_hash, size_t len);

#ifdef __cplusplus
}
#endif
