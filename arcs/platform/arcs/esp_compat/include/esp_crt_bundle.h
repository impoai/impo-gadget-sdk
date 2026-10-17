/*
 * ESP-IDF esp_crt_bundle.h on ARCS. Every TLS connection here (plat_tls.c)
 * already verifies the peer against the embedded roots in ca_bundle.c, with
 * SNI and the hostname check, so attaching the bundle has nothing left to do.
 */
#pragma once

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_crt_bundle_attach(void *conf);

#ifdef __cplusplus
}
#endif
