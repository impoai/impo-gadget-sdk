/*
 * ESP-IDF esp_tls and the two lwIP socket calls of the shared link (esp32/main), on
 * plat_tls.c. See include/esp_tls.h and include/lwip/sockets.h.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

#include "FreeRTOS.h"
#include "task.h"

#include "sysutils.h"

#include "platform_arcs.h"
#include "esp_compat.h"
#include "esp_tls.h"
#include "lwip/sockets.h"

#define TAG "esp_tls"
#include "lisa_log.h"

/* lwIP's recv() flags (lwip/sockets.h); SAL passes them through to lwIP. */
#define LWIP_MSG_PEEK     0x01
#define LWIP_MSG_DONTWAIT 0x08
int recv(int s, void *mem, size_t len, int flags);

#define MAX_CONNS 2

struct esp_tls {
    plat_tls_t *conn;
    int fd;
    bool nonblock;
};

/* Live connections, for lwip_fcntl() by socket. */
static esp_tls_t *s_conns[MAX_CONNS];

/* The newest connection: what `link` reports about the session. */
static volatile uint64_t s_last_rx_ms;
static __psram_bss__ char s_last_event[48];

static void track(esp_tls_t *t, bool add)
{
    taskENTER_CRITICAL();
    for (int i = 0; i < MAX_CONNS; i++) {
        if (add ? s_conns[i] == NULL : s_conns[i] == t) {
            s_conns[i] = add ? t : NULL;
            break;
        }
    }
    taskEXIT_CRITICAL();
}

static esp_tls_t *by_fd(int fd)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        if (s_conns[i] != NULL && s_conns[i]->fd == fd) {
            return s_conns[i];
        }
    }
    return NULL;
}

esp_tls_t *esp_tls_init(void)
{
    esp_tls_t *t = calloc(1, sizeof(*t));
    if (t != NULL) {
        t->fd = -1;
    }
    return t;
}

int esp_tls_conn_new_sync(const char *hostname, int hostlen, int port, const esp_tls_cfg_t *cfg, esp_tls_t *tls)
{
    char host[128];
    if (tls == NULL || hostname == NULL || hostlen <= 0 || hostlen >= (int)sizeof(host)) {
        return -1;
    }
    memcpy(host, hostname, (size_t)hostlen);
    host[hostlen] = '\0';
    esp_compat_wdt_alive("tls_connect");
    int err = 0;
    uint32_t timeout = cfg != NULL && cfg->timeout_ms > 0 ? (uint32_t)cfg->timeout_ms : 10000;
    tls->conn = plat_tls_connect(host, (uint16_t)port, timeout, &err);
    esp_compat_wdt_alive("tls_connected");
    if (tls->conn == NULL) {
        snprintf(s_last_event, sizeof(s_last_event), "tls connect -0x%04x", err < 0 ? -err : err);
        return -1;
    }
    tls->fd = plat_tls_fd(tls->conn);
    tls->nonblock = false;
    track(tls, true);
    s_last_rx_ms = plat_uptime_ms();
    snprintf(s_last_event, sizeof(s_last_event), "connected");
    return 1;
}

ssize_t esp_tls_conn_read(esp_tls_t *tls, void *data, size_t datalen)
{
    if (tls == NULL || tls->conn == NULL) {
        return -1;
    }
    esp_compat_wdt_alive("tls_read");
    int n = plat_tls_read(tls->conn, data, datalen, tls->nonblock ? 0 : ESP_COMPAT_TLS_BLOCK_SLICE_MS);
    if (n == PLAT_TLS_TIMEOUT) {
        return ESP_TLS_ERR_SSL_WANT_READ;
    }
    if (n > 0) {
        s_last_rx_ms = plat_uptime_ms();
    } else if (n == 0) {
        snprintf(s_last_event, sizeof(s_last_event), "peer closed");
    } else {
        snprintf(s_last_event, sizeof(s_last_event), "tls read -0x%04x", -n);
    }
    return n;
}

ssize_t esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t datalen)
{
    if (tls == NULL || tls->conn == NULL) {
        return -1;
    }
    esp_compat_wdt_alive("tls_write");
    int n = plat_tls_write(tls->conn, data, datalen);
    if (n < 0) {
        snprintf(s_last_event, sizeof(s_last_event), "tls write -0x%04x", -n);
    }
    return n;
}

int esp_tls_conn_destroy(esp_tls_t *tls)
{
    if (tls == NULL) {
        return -1;
    }
    if (tls->conn != NULL) {
        track(tls, false);
        plat_tls_close(tls->conn);   /* closes the SAL socket with closesocket() */
    }
    free(tls);
    return 0;
}

esp_err_t esp_tls_get_conn_sockfd(esp_tls_t *tls, int *sockfd)
{
    if (tls == NULL || sockfd == NULL || tls->conn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *sockfd = tls->fd;
    return ESP_OK;
}

ssize_t esp_tls_get_bytes_avail(esp_tls_t *tls)
{
    if (tls == NULL || tls->conn == NULL) {
        return -1;
    }
    return (ssize_t)plat_tls_pending(tls->conn);
}

uint32_t esp_compat_tls_rx_quiet_ms(void)
{
    return (uint32_t)(plat_uptime_ms() - s_last_rx_ms);
}

void esp_compat_tls_last_event(char *out, size_t cap)
{
    snprintf(out, cap, "%s", s_last_event);
}

/* ---- lwip_fcntl / lwip_select ---- */

int esp_compat_fcntl(int fd, int cmd, int val)
{
    esp_tls_t *t = by_fd(fd);
    if (t == NULL) {
        errno = EBADF;
        return -1;
    }
    if (cmd == F_GETFL) {
        return t->nonblock ? O_NONBLOCK : 0;
    }
    if (cmd == F_SETFL) {
        t->nonblock = (val & O_NONBLOCK) != 0;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int esp_compat_select(int maxfdp1, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
{
    (void)writefds;
    (void)exceptfds;
    uint64_t start = plat_uptime_ms();
    uint64_t wait_ms = timeout != NULL ? (uint64_t)timeout->tv_sec * 1000 + (uint64_t)timeout->tv_usec / 1000 : UINT64_MAX;
    fd_set want;
    FD_ZERO(&want);
    if (readfds != NULL) {
        want = *readfds;
        FD_ZERO(readfds);
    }
    for (;;) {
        int ready = 0;
        for (int fd = 0; readfds != NULL && fd < maxfdp1; fd++) {
            char c;
            /* Readable: data waiting, or the end of the stream (recv returns 0). */
            if (FD_ISSET(fd, &want) && recv(fd, &c, 1, LWIP_MSG_PEEK | LWIP_MSG_DONTWAIT) >= 0) {
                FD_SET(fd, readfds);
                ready++;
            }
        }
        if (ready > 0 || plat_uptime_ms() - start >= wait_ms) {
            return ready;
        }
        vTaskDelay(1);
    }
}
