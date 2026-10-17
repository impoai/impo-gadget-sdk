/*
 * Impo BLE setup v5 crypto (community mode): P-256 ECDH, HKDF-SHA256 and
 * AES-256-GCM records. Matches impo-gadget-sdk linux/src/impogadget/pairing.py.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAIR_PUB_LEN      65   /* uncompressed P-256 point 04||X||Y */
#define PAIR_PRIV_LEN     32
#define PAIR_NONCE_LEN    16
#define PAIR_HASH_LEN     32
#define PAIR_SID_LEN      16
#define PAIR_TAG_LEN      16

/* Random bytes for key generation; the firmware and host tests supply it. */
int pairing_rng(void *ctx, unsigned char *out, size_t len);

/* Unpadded base64url. encode returns the length written (excluding NUL) or -1.
 * decode is strict: rejects other characters and len % 4 == 1; returns bytes or -1. */
int b64url_encode(const uint8_t *in, size_t n, char *out, size_t cap);
int b64url_decode(const char *in, uint8_t *out, size_t cap);

int pair_keygen(uint8_t priv[PAIR_PRIV_LEN], uint8_t pub[PAIR_PUB_LEN]);
int pair_pub_from_priv(const uint8_t priv[PAIR_PRIV_LEN], uint8_t pub[PAIR_PUB_LEN]);
/* Validates the peer point; secret is the X coordinate of the shared point. */
int pair_ecdh(const uint8_t priv[PAIR_PRIV_LEN], const uint8_t peer[PAIR_PUB_LEN], uint8_t secret[32]);

int pair_sha256(const void *data, size_t len, uint8_t out[PAIR_HASH_LEN]);

typedef struct {
    uint8_t rx_key[32];      /* device decrypts (mobile->device) */
    uint8_t tx_key[32];      /* device encrypts (device->mobile) */
    char session_id[32];     /* base64url of 16 bytes */
    uint64_t rx_counter;
    uint64_t tx_counter;
} pair_session_t;

int pair_derive(const uint8_t ecdh[32], const uint8_t mobile_nonce[PAIR_NONCE_LEN],
                const uint8_t device_nonce[PAIR_NONCE_LEN], const uint8_t transcript_hash[PAIR_HASH_LEN],
                pair_session_t *s);

/* Encrypt plaintext as the next device->mobile record. Outputs base64url ciphertext and tag. */
int pair_seal(pair_session_t *s, const uint8_t *pt, size_t len, uint64_t *counter_out,
              char *ct_b64, size_t ct_cap, char tag_b64[32]);

/* Decrypt the next mobile->device record. Enforces session id and strict counter order.
 * Returns plaintext length (NUL-terminated in pt) or -1. */
int pair_open(pair_session_t *s, const char *session_id, const char *counter, const char *ct_b64,
              const char *tag_b64, uint8_t *pt, size_t pt_cap);

void pair_wipe(void *p, size_t n);

#ifdef __cplusplus
}
#endif
