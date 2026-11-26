/* Exercise actual mbedTLS configuration; replace only entropy, socket and handshake. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
static int fake_seed(mbedtls_ctr_drbg_context *ctx,
                     int (*entropy)(void *, unsigned char *, size_t), void *arg,
                     const unsigned char *custom, size_t len)
{ (void)ctx; (void)entropy; (void)arg; (void)custom; (void)len; return 0; }
static char dial_host[128], dial_port[8];
static int handshake_result;
static int fake_handshake(mbedtls_ssl_context *ssl)
{
    assert(ssl->conf->authmode == MBEDTLS_SSL_VERIFY_REQUIRED);
    return handshake_result;
}
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define mbedtls_ssl_handshake fake_handshake
#define mbedtls_ctr_drbg_seed fake_seed
#include "../../platform/arcs/plat_tls.c"
#undef mbedtls_ssl_handshake
#undef mbedtls_ctr_drbg_seed

const char plat_ca_bundle_pem[] = "";
uint64_t plat_uptime_ms(void) { return 0; }
bool plat_time_synced(void) { return true; }
void vTaskDelay(unsigned ms) { (void)ms; }
void test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
int closesocket(int fd) { (void)fd; return 0; }
/* Allows this harness to compose with the separate TCP_NODELAY change (#6). */
int sal_setsockopt(int fd, int level, int option, const void *value, socklen_t size)
{ (void)fd; (void)level; (void)option; (void)value; (void)size; return 0; }
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

int main(void)
{
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
}
