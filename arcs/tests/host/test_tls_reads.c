/* Execute plat_tls_read itself with deterministic mbedTLS outcomes and clock. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
static int fake_seed(mbedtls_ctr_drbg_context *ctx,
                     int (*entropy)(void *, unsigned char *, size_t), void *arg,
                     const unsigned char *custom, size_t len)
{ (void)ctx; (void)entropy; (void)arg; (void)custom; (void)len; return 0; }
static unsigned calls, delays;
static uint64_t now_ms;
static int outcomes[8];
static size_t outcomes_n;
static unsigned socket_option_calls;
static int socket_option_result;
static char dial_host[128], dial_port[8];
static int handshake_result;
static int fake_handshake(mbedtls_ssl_context *ssl)
{
    assert(ssl->conf->authmode == MBEDTLS_SSL_VERIFY_REQUIRED);
    return handshake_result;
}
static int fake_ssl_read(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len)
{
    (void)ssl; (void)buf; (void)len;
    assert(calls < 50); /* A regression must fail, rather than hang the host test. */
    unsigned i = calls++;
    return outcomes[i < outcomes_n ? i : outcomes_n - 1];
}
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define mbedtls_ssl_read fake_ssl_read
#define mbedtls_ssl_handshake fake_handshake
#define mbedtls_ctr_drbg_seed fake_seed
#include "../../platform/arcs/plat_tls.c"
#undef mbedtls_ssl_read
#undef mbedtls_ssl_handshake
#undef mbedtls_ctr_drbg_seed

const char plat_ca_bundle_pem[] = "";
uint64_t plat_uptime_ms(void) { return now_ms; }
bool plat_time_synced(void) { return true; }
void vTaskDelay(unsigned ms) { now_ms += ms; delays += ms; }
void test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
int closesocket(int fd) { (void)fd; return 0; }
int sal_setsockopt(int fd, int level, int option, const void *value, socklen_t size)
{
    assert(fd == 123 && level == IPPROTO_TCP && option == TCP_NODELAY);
    assert(size == sizeof(int) && *(const int *)value == 1);
    socket_option_calls++;
    return socket_option_result;
}
int mbedtls_hardware_poll(void *d, unsigned char *o, size_t n, size_t *got)
{ (void)d; memset(o, 0, n); *got = n; return 0; }
int mbedtls_net_connect(mbedtls_net_context *c, const char *h, const char *p, int proto)
{ assert(proto == MBEDTLS_NET_PROTO_TCP); c->fd=123;
  snprintf(dial_host,sizeof(dial_host),"%s",h);snprintf(dial_port,sizeof(dial_port),"%s",p);return 0; }
void mbedtls_net_init(mbedtls_net_context *c) { c->fd = -1; }
void mbedtls_net_free(mbedtls_net_context *c) { (void)c; }
int mbedtls_net_set_nonblock(mbedtls_net_context *c) { (void)c; return 0; }
int mbedtls_net_send(void *c, const unsigned char *b, size_t n)
{ (void)c; (void)b; (void)n; return -1; }
int mbedtls_net_recv_timeout(void *c, unsigned char *b, size_t n, uint32_t ms)
{ (void)c; (void)b; (void)n; (void)ms; return -1; }

static void sequence(int a, int b, size_t n)
{ outcomes[0] = a; outcomes[1] = b; outcomes_n = n; calls = delays = 0; now_ms = 0; }
int main(void)
{
    plat_tls_t t = {0}; unsigned char buf[64];
    t.net.fd = 123;
    tune_tcp(&t.net);
    assert(socket_option_calls == 1);
    socket_option_result = -1;
    tune_tcp(&t.net); /* An unavailable optimization does not close the connection. */
    assert(socket_option_calls == 2);
    sequence(MBEDTLS_ERR_SSL_WANT_READ, 0, 1);
    assert(plat_tls_read(&t, buf, sizeof(buf), 0) == PLAT_TLS_TIMEOUT);
    assert(calls == 1 && delays == 0);
    sequence(MBEDTLS_ERR_SSL_WANT_WRITE, 0, 1);
    assert(plat_tls_read(&t, buf, sizeof(buf), 0) == PLAT_TLS_TIMEOUT);
    assert(calls == 1 && delays == 0);
    sequence(MBEDTLS_ERR_SSL_WANT_READ, 0, 1);
    assert(plat_tls_read(&t, buf, sizeof(buf), 25) == PLAT_TLS_TIMEOUT);
    assert(delays == 25 && calls == 3);
    sequence(MBEDTLS_ERR_SSL_WANT_READ, 12, 2);
    assert(plat_tls_read(&t, buf, sizeof(buf), 100) == 12);
    assert(calls == 2 && delays == 10);
    sequence(MBEDTLS_ERR_SSL_TIMEOUT, 0, 1);
    assert(plat_tls_read(&t, buf, sizeof(buf), 100) == PLAT_TLS_TIMEOUT && delays == 0);
    sequence(MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY, 0, 1);
    assert(plat_tls_read(&t, buf, sizeof(buf), 0) == 0);
    sequence(-0x7400, 0, 1);
    assert(plat_tls_read(&t, buf, sizeof(buf), 0) == -0x7400);
    /* Exercise the real TLS configuration while replacing the network handshake.
     * The socket peer and verified/SNI identity must stay independent. */
    s_ca_ready = true;
    int err = 0;
    plat_tls_t *named = plat_tls_connect_named("gadget.itoy.cn", 9443, "gw.itoy.cn", 5000, &err);
    assert(named && !err && !strcmp(dial_host,"gadget.itoy.cn") && !strcmp(dial_port,"9443"));
    assert(!strcmp(named->ssl.hostname,"gw.itoy.cn"));
    plat_tls_close(named);
    named = plat_tls_connect("custom.example",443,5000,&err);
    assert(named && !strcmp(named->ssl.hostname,"custom.example"));plat_tls_close(named);
    handshake_result = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    assert(!plat_tls_connect_named("gadget.itoy.cn",9443,"gw.itoy.cn",5000,&err));
    assert(err == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED);
    puts("TLS route: separate peer/SNI, required certificate verification and failure propagation passed");
    puts("TLS reads: nonblocking WANT_READ/WANT_WRITE, deadline, data, timeout, close and errors passed");
}
