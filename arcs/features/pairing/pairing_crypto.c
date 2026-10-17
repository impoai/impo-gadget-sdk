/* Impo BLE setup v5 crypto. See pairing_crypto.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/bignum.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

#include "pairing_crypto.h"

#define RECORD_LABEL     "hatch-link ble setup v1"
#define SESSION_ID_LABEL "hatch-link session id v1"

void pair_wipe(void *p, size_t n)
{
    mbedtls_platform_zeroize(p, n);
}

/* ---- base64url ---- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int b64url_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t need = (n / 3) * 4 + (n % 3 ? n % 3 + 1 : 0);
    if (need + 1 > cap) {
        return -1;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = B64[(v >> 6) & 63];
        if (i + 2 < n) out[o++] = B64[v & 63];
    }
    out[o] = '\0';
    return (int)o;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

int b64url_decode(const char *in, uint8_t *out, size_t cap)
{
    size_t len = strlen(in);
    if (len % 4 == 1) {
        return -1;
    }
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64_val(in[i]);
        if (v < 0) {
            return -1;
        }
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) {
                return -1;
            }
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)o;
}

/* ---- hashes ---- */

int pair_sha256(const void *data, size_t len, uint8_t out[PAIR_HASH_LEN])
{
    return mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), data, len, out) == 0 ? 0 : -1;
}

static int sha256_parts(const void *a, size_t al, const void *b, size_t bl, const void *c, size_t cl,
                        uint8_t out[PAIR_HASH_LEN])
{
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int ok = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) == 0 &&
             mbedtls_md_starts(&ctx) == 0 && mbedtls_md_update(&ctx, a, al) == 0 &&
             mbedtls_md_update(&ctx, b, bl) == 0 && mbedtls_md_update(&ctx, c, cl) == 0 &&
             mbedtls_md_finish(&ctx, out) == 0;
    mbedtls_md_free(&ctx);
    return ok ? 0 : -1;
}

/* ---- P-256 ---- */

int pair_keygen(uint8_t priv[PAIR_PRIV_LEN], uint8_t pub[PAIR_PUB_LEN])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);
    size_t olen = 0;
    int ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
             mbedtls_ecp_gen_keypair(&grp, &d, &q, pairing_rng, NULL) == 0 &&
             mbedtls_mpi_write_binary(&d, priv, PAIR_PRIV_LEN) == 0 &&
             mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, pub, PAIR_PUB_LEN) == 0 &&
             olen == PAIR_PUB_LEN;
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok ? 0 : -1;
}

int pair_pub_from_priv(const uint8_t priv[PAIR_PRIV_LEN], uint8_t pub[PAIR_PUB_LEN])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);
    size_t olen = 0;
    int ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
             mbedtls_mpi_read_binary(&d, priv, PAIR_PRIV_LEN) == 0 &&
             mbedtls_ecp_mul(&grp, &q, &d, &grp.G, pairing_rng, NULL) == 0 &&
             mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, pub, PAIR_PUB_LEN) == 0 &&
             olen == PAIR_PUB_LEN;
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok ? 0 : -1;
}

int pair_ecdh(const uint8_t priv[PAIR_PRIV_LEN], const uint8_t peer[PAIR_PUB_LEN], uint8_t secret[32])
{
    if (peer[0] != 0x04) {
        return -1;
    }
    mbedtls_ecp_group grp;
    mbedtls_mpi d, z;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&q);
    int ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
             mbedtls_ecp_point_read_binary(&grp, &q, peer, PAIR_PUB_LEN) == 0 &&
             mbedtls_ecp_check_pubkey(&grp, &q) == 0 &&
             mbedtls_mpi_read_binary(&d, priv, PAIR_PRIV_LEN) == 0 &&
             mbedtls_ecdh_compute_shared(&grp, &z, &q, &d, pairing_rng, NULL) == 0 &&
             mbedtls_mpi_write_binary(&z, secret, 32) == 0;
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok ? 0 : -1;
}

/* ---- session keys and records ---- */

int pair_derive(const uint8_t ecdh[32], const uint8_t mobile_nonce[PAIR_NONCE_LEN],
                const uint8_t device_nonce[PAIR_NONCE_LEN], const uint8_t th[PAIR_HASH_LEN], pair_session_t *s)
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t salt[32], secret[32], sid[32];
    memset(s, 0, sizeof(*s));
    int ok = sha256_parts(mobile_nonce, PAIR_NONCE_LEN, device_nonce, PAIR_NONCE_LEN, th, PAIR_HASH_LEN, salt) == 0 &&
             mbedtls_hkdf(md, salt, sizeof(salt), ecdh, 32, (const uint8_t *)RECORD_LABEL, strlen(RECORD_LABEL),
                          secret, sizeof(secret)) == 0 &&
             mbedtls_hkdf_expand(md, secret, sizeof(secret), (const uint8_t *)"mobile->device", 14, s->rx_key, 32) ==
                 0 &&
             mbedtls_hkdf_expand(md, secret, sizeof(secret), (const uint8_t *)"device->mobile", 14, s->tx_key, 32) ==
                 0 &&
             sha256_parts(SESSION_ID_LABEL, strlen(SESSION_ID_LABEL), th, PAIR_HASH_LEN, ecdh, 32, sid) == 0 &&
             b64url_encode(sid, PAIR_SID_LEN, s->session_id, sizeof(s->session_id)) > 0;
    pair_wipe(secret, sizeof(secret));
    if (!ok) {
        pair_wipe(s, sizeof(*s));
    }
    return ok ? 0 : -1;
}

static void record_nonce(uint8_t dir, uint64_t counter, uint8_t nonce[12])
{
    nonce[0] = dir;
    nonce[1] = nonce[2] = nonce[3] = 0;
    for (int i = 0; i < 8; i++) {
        nonce[4 + i] = (uint8_t)(counter >> (8 * (7 - i)));
    }
}

static int record_aad(const char *sid, const char *arrow, uint64_t counter, char *aad, size_t cap)
{
    return snprintf(aad, cap, "%s|%s|%s|%llu", RECORD_LABEL, sid, arrow, (unsigned long long)counter);
}

int pair_seal(pair_session_t *s, const uint8_t *pt, size_t len, uint64_t *counter_out, char *ct_b64, size_t ct_cap,
              char tag_b64[32])
{
    uint8_t nonce[12], tag[PAIR_TAG_LEN];
    char aad[96];
    uint8_t *ct = malloc(len ? len : 1);
    if (ct == NULL) {
        return -1;
    }
    uint64_t counter = s->tx_counter;
    record_nonce(1, counter, nonce);
    int aad_len = record_aad(s->session_id, "d2m", counter, aad, sizeof(aad));
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s->tx_key, 256) == 0 &&
             mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len, nonce, sizeof(nonce), (const uint8_t *)aad,
                                       (size_t)aad_len, pt, ct, sizeof(tag), tag) == 0 &&
             b64url_encode(ct, len, ct_b64, ct_cap) >= 0 && b64url_encode(tag, sizeof(tag), tag_b64, 32) > 0;
    mbedtls_gcm_free(&gcm);
    free(ct);
    if (!ok) {
        return -1;
    }
    *counter_out = counter;
    s->tx_counter++;
    return 0;
}

int pair_open(pair_session_t *s, const char *session_id, const char *counter, const char *ct_b64, const char *tag_b64,
              uint8_t *pt, size_t pt_cap)
{
    if (strcmp(session_id, s->session_id) != 0) {
        return -1;
    }
    char expect[24];
    snprintf(expect, sizeof(expect), "%llu", (unsigned long long)s->rx_counter);
    if (strcmp(counter, expect) != 0) {
        return -1;
    }
    uint8_t tag[PAIR_TAG_LEN + 2];
    if (b64url_decode(tag_b64, tag, sizeof(tag)) != PAIR_TAG_LEN) {
        return -1;
    }
    size_t ct_max = strlen(ct_b64) * 3 / 4 + 1;
    if (ct_max >= pt_cap) {
        return -1;
    }
    uint8_t *ct = malloc(ct_max);
    if (ct == NULL) {
        return -1;
    }
    int ct_len = b64url_decode(ct_b64, ct, ct_max);
    uint8_t nonce[12];
    char aad[96];
    record_nonce(0, s->rx_counter, nonce);
    int aad_len = record_aad(s->session_id, "m2d", s->rx_counter, aad, sizeof(aad));
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int ok = ct_len >= 0 && mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s->rx_key, 256) == 0 &&
             mbedtls_gcm_auth_decrypt(&gcm, (size_t)ct_len, nonce, sizeof(nonce), (const uint8_t *)aad,
                                      (size_t)aad_len, tag, PAIR_TAG_LEN, ct, pt) == 0;
    mbedtls_gcm_free(&gcm);
    free(ct);
    if (!ok) {
        return -1;
    }
    pt[ct_len] = '\0';
    s->rx_counter++;
    return ct_len;
}
