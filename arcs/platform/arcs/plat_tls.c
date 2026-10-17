/*
 * TLS 1.2 client on mbedtls over a BSD socket.
 * The peer chain is verified against plat_ca_bundle_pem with SNI and hostname check.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "platform_arcs.h"

/* SAL (components/lisa_net); not declared by the headers mbedtls pulls in. */
int closesocket(int s);

#define TAG "plat_tls"
#include "lisa_log.h"

/*
 * The socket is non-blocking: the SDK's mbedtls_net_recv_timeout() only honours
 * its timeout that way (on a blocking socket recv() waits forever). Writes that
 * cannot go out yet are retried every POLL_MS for at most WRITE_TIMEOUT_MS.
 */
#define POLL_MS           10
#define WRITE_TIMEOUT_MS  15000

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);

struct plat_tls {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
};

/* Parsed once and shared read-only by every connection. */
static mbedtls_x509_crt s_ca;
static bool s_ca_ready;
static SemaphoreHandle_t s_ca_lock;

int plat_random(void *buf, size_t len)
{
    unsigned char *p = buf;
    while (len > 0) {
        size_t got = 0;
        if (mbedtls_hardware_poll(NULL, p, len, &got) != 0 || got == 0) {
            return -1;
        }
        p += got;
        len -= got;
    }
    return 0;
}

static int load_ca(void)
{
    if (s_ca_lock == NULL) {
        taskENTER_CRITICAL();
        if (s_ca_lock == NULL) {
            s_ca_lock = xSemaphoreCreateMutex();
        }
        taskEXIT_CRITICAL();
    }
    xSemaphoreTake(s_ca_lock, portMAX_DELAY);
    int ret = 0;
    if (!s_ca_ready) {
        mbedtls_x509_crt_init(&s_ca);
        ret = mbedtls_x509_crt_parse(&s_ca, (const unsigned char *)plat_ca_bundle_pem,
                                     strlen(plat_ca_bundle_pem) + 1);
        if (ret == 0) {
            s_ca_ready = true;
        } else {
            LISA_LOGE(TAG, "ca bundle parse: -0x%04x", -ret);
            mbedtls_x509_crt_free(&s_ca);
        }
    }
    xSemaphoreGive(s_ca_lock);
    return ret;
}

static void log_error(const char *what, int ret)
{
    char msg[96];
    mbedtls_strerror(ret, msg, sizeof(msg));
    LISA_LOGE(TAG, "%s: -0x%04x %s (errno %d)", what, -ret, msg, errno);
}

plat_tls_t *plat_tls_connect(const char *host, uint16_t port, uint32_t timeout_ms, int *err)
{
    int ret;
    if (err != NULL) {
        *err = 0;
    }
    if (!plat_time_synced()) {
        LISA_LOGE(TAG, "clock not synced; refusing TLS to %s", host);
        if (err != NULL) {
            *err = -1;
        }
        return NULL;
    }
    if ((ret = load_ca()) != 0) {
        if (err != NULL) {
            *err = ret;
        }
        return NULL;
    }

    plat_tls_t *t = calloc(1, sizeof(*t));
    if (t == NULL) {
        if (err != NULL) {
            *err = -1;
        }
        return NULL;
    }
    mbedtls_net_init(&t->net);
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_entropy_init(&t->entropy);

    static const char pers[] = "impo-gadget-tls";
    if ((ret = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy,
                                     (const unsigned char *)pers, sizeof(pers) - 1)) != 0) {
        log_error("drbg seed", ret);
        goto fail;
    }

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    if ((ret = mbedtls_net_connect(&t->net, host, port_str, MBEDTLS_NET_PROTO_TCP)) != 0) {
        log_error("tcp connect", ret);
        goto fail;
    }
    if ((ret = mbedtls_net_set_nonblock(&t->net)) != 0) {
        LISA_LOGE(TAG, "set nonblock: %d", ret);
        goto fail;
    }

    if ((ret = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        log_error("config", ret);
        goto fail;
    }
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&t->conf, &s_ca, NULL);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    mbedtls_ssl_conf_read_timeout(&t->conf, timeout_ms);

    if ((ret = mbedtls_ssl_setup(&t->ssl, &t->conf)) != 0 ||
        (ret = mbedtls_ssl_set_hostname(&t->ssl, host)) != 0) {
        log_error("setup", ret);
        goto fail;
    }
    mbedtls_ssl_set_bio(&t->ssl, &t->net, mbedtls_net_send, NULL, mbedtls_net_recv_timeout);

    uint64_t hs_start = plat_uptime_ms();
    while ((ret = mbedtls_ssl_handshake(&t->ssl)) != 0) {
        if ((ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) &&
            plat_uptime_ms() - hs_start > 2 * (uint64_t)timeout_ms) {
            LISA_LOGE(TAG, "handshake took over %u ms", (unsigned)(2 * timeout_ms));
            ret = MBEDTLS_ERR_SSL_TIMEOUT;
            goto fail;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        } else {
            log_error("handshake", ret);
            uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
            if (flags != 0) {
                char info[160];
                mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
                LISA_LOGE(TAG, "verify: %s", info);
            }
            goto fail;
        }
    }
    LISA_LOGI(TAG, "connected %s:%u %s", host, port, mbedtls_ssl_get_ciphersuite(&t->ssl));
    return t;

fail:
    if (err != NULL) {
        *err = ret;
    }
    plat_tls_close(t);
    return NULL;
}

int plat_tls_write(plat_tls_t *t, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    size_t left = len;
    uint64_t start = plat_uptime_ms();
    while (left > 0) {
        int ret = mbedtls_ssl_write(&t->ssl, p, left);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (plat_uptime_ms() - start > WRITE_TIMEOUT_MS) {
                LISA_LOGE(TAG, "write stalled %u ms with %u bytes left", WRITE_TIMEOUT_MS, (unsigned)left);
                return MBEDTLS_ERR_SSL_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }
        if (ret < 0) {
            log_error("write", ret);
            return ret;
        }
        p += ret;
        left -= (size_t)ret;
    }
    return (int)len;
}

int plat_tls_read(plat_tls_t *t, void *buf, size_t len, uint32_t timeout_ms)
{
    mbedtls_ssl_conf_read_timeout(&t->conf, timeout_ms);
    for (;;) {
        int ret = mbedtls_ssl_read(&t->ssl, buf, len);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            /* A record that needs more bytes; the next read waits for them with the timeout. */
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_TIMEOUT) {
            return PLAT_TLS_TIMEOUT;
        }
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) {
            return 0;
        }
        if (ret < 0) {
            log_error("read", ret);
        }
        return ret;
    }
}

void plat_tls_close(plat_tls_t *t)
{
    if (t == NULL) {
        return;
    }
    mbedtls_ssl_close_notify(&t->ssl);
    /* mbedtls_net_free() calls close(), which here is newlib's stub and leaves the SAL socket
     * open: after 16 connections (CONFIG_SAL_SOCKETS_NUM) no socket can be made (ENFILE).
     * Close it the SAL way first. */
    if (t->net.fd >= 0) {
        closesocket(t->net.fd);
        t->net.fd = -1;
    }
    mbedtls_net_free(&t->net);
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    free(t);
}

int plat_tls_fd(const plat_tls_t *t)
{
    return t != NULL ? t->net.fd : -1;
}

size_t plat_tls_pending(plat_tls_t *t)
{
    return t != NULL ? mbedtls_ssl_get_bytes_avail(&t->ssl) : 0;
}
