/*
 * mbedtls 2.28 configuration for the gadget.
 *
 * Enables only what the device needs:
 *   - TLS 1.2 client to gadgets.impo.ai (Cloudflare, ECDSA P-256/P-384 chain, RSA cross-sign)
 *   - Noise_XX_25519_AESGCM_SHA256 (X25519, AES-GCM, SHA-256, HKDF)
 *   - the crypto the Wi-Fi supplicant port calls (bignum, ECP, ECDH, ECDSA, PK parse)
 * Selected by CONFIG_MBEDTLS_CFG_FILE in prj.conf.
 */
#ifndef GADGET_MBEDTLS_CONFIG_H
#define GADGET_MBEDTLS_CONFIG_H

#include <stddef.h>
#include <time.h>

/* ---- System ---- */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_GMTIME_R_ALT

/* The SDK builds bignum/ecp with CFG_HSU (hardware speed-up); its declarations come
 * from the port header, which mbedtls includes through MBEDTLS_PLATFORM_STD_MEM_HDR. */
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
#define MBEDTLS_PLATFORM_STD_MEM_HDR    "ls_mbedtls_platform.h"
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO snprintf

/* Allocations go to PSRAM (port/mem/ls_mem.c, CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC). */
void *ls_mbedtls_mem_calloc(size_t n, size_t size);
void ls_mbedtls_mem_free(void *ptr);
#define MBEDTLS_PLATFORM_STD_CALLOC ls_mbedtls_mem_calloc
#define MBEDTLS_PLATFORM_STD_FREE   ls_mbedtls_mem_free

/* Wall clock from SNTP; -1 before the first sync, so certificate checks fail closed. */
time_t plat_tls_time(time_t *out);
#define MBEDTLS_PLATFORM_TIME_MACRO plat_tls_time

/* Entropy from the hardware TRNG (port/csk_trng.c). */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* ---- Symmetric crypto and hashes ---- */
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA256_SMALLER
#define MBEDTLS_SHA512_C
#define MBEDTLS_HKDF_C
#define MBEDTLS_PKCS5_C

/* ---- Public key ---- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C

/* ---- X.509 ---- */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CHECK_KEY_USAGE
#define MBEDTLS_X509_CHECK_EXTENDED_KEY_USAGE

/* ---- TLS client (MBEDTLS_NET_C stays off: port/net_sockets.c supplies the BSD-socket glue) ---- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

#define MBEDTLS_ERROR_C

#include "mbedtls/check_config.h"

#endif /* GADGET_MBEDTLS_CONFIG_H */
