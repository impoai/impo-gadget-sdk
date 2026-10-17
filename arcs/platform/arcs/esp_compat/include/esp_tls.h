/*
 * ESP-IDF esp_tls on ARCS, built on plat_tls.c (mbedtls 2.28, the embedded
 * roots in ca_bundle.c, SNI and hostname check, the socket closed the SAL way).
 *
 * As in ESP-IDF, reads wait for data until the caller sets O_NONBLOCK on the
 * socket (lwip_fcntl); here a waiting read gives up after
 * ESP_COMPAT_TLS_BLOCK_SLICE_MS with ESP_TLS_ERR_SSL_WANT_READ, which every
 * caller in the shared link already retries until its own deadline.
 * Afterwards reads return ESP_TLS_ERR_SSL_WANT_READ at once when nothing is
 * there. Writes always complete (or fail) before returning.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_TLS_ERR_SSL_WANT_READ  (-0x6900)   /* MBEDTLS_ERR_SSL_WANT_READ */
#define ESP_TLS_ERR_SSL_WANT_WRITE (-0x6880)   /* MBEDTLS_ERR_SSL_WANT_WRITE */
#define ESP_COMPAT_TLS_BLOCK_SLICE_MS 100

typedef struct esp_tls esp_tls_t;

typedef struct {
    esp_err_t (*crt_bundle_attach)(void *conf);   /* accepted; plat_tls always verifies */
    int timeout_ms;                               /* TCP connect and TLS handshake */
} esp_tls_cfg_t;

esp_tls_t *esp_tls_init(void);
/* 1 when connected, -1 on failure (the handle stays valid for esp_tls_conn_destroy). */
int esp_tls_conn_new_sync(const char *hostname, int hostlen, int port, const esp_tls_cfg_t *cfg, esp_tls_t *tls);
ssize_t esp_tls_conn_read(esp_tls_t *tls, void *data, size_t datalen);
ssize_t esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t datalen);
int esp_tls_conn_destroy(esp_tls_t *tls);
esp_err_t esp_tls_get_conn_sockfd(esp_tls_t *tls, int *sockfd);
ssize_t esp_tls_get_bytes_avail(esp_tls_t *tls);

#ifdef __cplusplus
}
#endif
