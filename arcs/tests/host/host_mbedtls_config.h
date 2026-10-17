/* mbedtls config for host tests: just what pairing_crypto needs. */
#ifndef HOST_MBEDTLS_CONFIG_H
#define HOST_MBEDTLS_CONFIG_H
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_HKDF_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECDH_C
#define MBEDTLS_CTR_DRBG_C
#include "mbedtls/check_config.h"
#endif
