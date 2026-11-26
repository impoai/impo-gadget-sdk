/* Exercise the real ARCS HTTP and ESP TLS adapters with a captured TLS boundary.
 * Credentials here are synthetic. No live account or network is needed. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "platform_arcs.h"
#include "http_client.h"
#include "esp_http_client.h"
#include "esp_tls.h"

static char peer[128], identity[128], request[4096];
static uint16_t peer_port;
static uint32_t deadline;
static size_t written, received;
static int dummy;
static const char reply[] = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 2\r\n\r\n{}";
plat_tls_t *plat_tls_connect_named(const char *host, uint16_t port, const char *name,
                                   uint32_t timeout, int *err)
{
    snprintf(peer, sizeof(peer), "%s", host);
    snprintf(identity, sizeof(identity), "%s", name ? name : host);
    peer_port = port; deadline = timeout; written = received = 0; request[0] = 0;
    *err = 0; return (plat_tls_t *)&dummy;
}
int plat_tls_write(plat_tls_t *tls, const void *buf, size_t len)
{
    (void)tls; assert(written + len < sizeof(request));
    memcpy(request + written, buf, len); written += len; request[written] = 0;
    return (int)len;
}
int plat_tls_read(plat_tls_t *tls, void *buf, size_t len, uint32_t timeout)
{
    (void)tls; assert(timeout == deadline);
    size_t left = sizeof(reply) - 1 - received;
    if (len > left) len = left;
    memcpy(buf, reply + received, len); received += len; return (int)len;
}
void plat_tls_close(plat_tls_t *tls) { assert(tls == (plat_tls_t *)&dummy); }
int plat_tls_fd(const plat_tls_t *tls) { (void)tls; return 7; }
size_t plat_tls_pending(plat_tls_t *tls) { (void)tls; return 0; }
const char *plat_firmware_version(void) { return "host-test"; }
uint64_t plat_uptime_ms(void) { return 0; }
void vTaskDelay(unsigned ms) { (void)ms; }
void esp_compat_wdt_alive(const char *where) { (void)where; }
void test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }

static void single_host(const char *expected)
{
    unsigned count = 0;
    for (const char *p = request; p && *p;) {
        if (!strncasecmp(p, "Host:", 5)) {
            count++;
            assert(!strncmp(p + 6, expected, strlen(expected)));
            assert(!strncmp(p + 6 + strlen(expected), "\r\n", 2));
        }
        const char *next = strstr(p, "\r\n"); p = next ? next + 2 : NULL;
    }
    assert(count == 1);
}
static void http_case(const char *url, const char *name, const char *host, unsigned port,
                      unsigned timeout, bool explicit_host)
{
    esp_http_client_config_t cfg = { .url=url, .common_name=name, .timeout_ms=(int)timeout,
                                    .disable_auto_redirect=true };
    esp_http_client_handle_t c = esp_http_client_init(&cfg); assert(c);
    if (explicit_host) assert(esp_http_client_set_header(c, "hOsT", "gw.itoy.cn") == ESP_OK);
    assert(esp_http_client_set_header(c, "Authorization", "Bearer synthetic-test-only") == ESP_OK);
    assert(esp_http_client_perform(c) == ESP_OK);
    assert(esp_http_client_get_status_code(c) == 401);
    assert(!strcmp(peer, host) && peer_port == port && deadline == timeout);
    assert(!strcmp(identity, name ? name : host));
    assert(strstr(request, "Authorization: Bearer synthetic-test-only\r\n"));
    single_host(explicit_host ? "gw.itoy.cn" : port == 9443 ? "gadget.itoy.cn:9443" : host);
    assert(esp_http_client_cleanup(c) == ESP_OK);
}
int main(void)
{
    http_case("https://gadget.itoy.cn:9443/api/v1/gadgets/vms", "gw.itoy.cn",
              "gadget.itoy.cn", 9443, 15000, true);
    http_case("https://gadget.itoy.cn:9443/where", NULL, "gadget.itoy.cn", 9443, 5000, false);
    http_case("https://gadgets.impo.ai/api/v1/gadgets/vms", NULL, "gadgets.impo.ai", 443, 10000, false);
    http_case("https://custom.example/vms", NULL, "custom.example", 443, 10000, false);
    http_url_t u;
    assert(!http_parse_url("https://gadget.itoy.cn:9443?test=1#ignored", &u));
    assert(u.port == 9443 && !strcmp(u.path, "/?test=1"));
    const char *bad[] = { "https://x:0/", "https://x:65536/", "https://x:abc/", "https://x:/",
                          "https://user@x/", "https://x/\r\nInjected: x", "http://x/" };
    for (unsigned i=0; i<sizeof(bad)/sizeof(bad[0]); i++) assert(http_parse_url(bad[i], &u) < 0);
    esp_tls_cfg_t cfg = { .common_name="gw.itoy.cn", .timeout_ms=12000 };
    esp_tls_t *tls = esp_tls_init(); assert(tls);
    assert(esp_tls_conn_new_sync("gadget.itoy.cn", (int)strlen("gadget.itoy.cn"), 9443, &cfg, tls) == 1);
    assert(!strcmp(peer,"gadget.itoy.cn") && peer_port==9443 && deadline==12000);
    assert(!strcmp(identity,"gw.itoy.cn"));
    esp_tls_conn_destroy(tls);
    tls=esp_tls_init(); assert(tls);
    assert(esp_tls_conn_new_sync("custom.example", 14, 443, NULL, tls) == 1);
    assert(!strcmp(identity,"custom.example") && deadline==10000);
    esp_tls_conn_destroy(tls);
    puts("ARCS cloud transport: relay peer, TLS identity, single Host, timeouts, direct/custom and HTTP status passed");
}
