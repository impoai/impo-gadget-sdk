/* Host tests for the link glue's pure parts: link/link_glue_logic.c and
 * platform/arcs/esp_compat/esp_compat_url.c (esp_http_client redirects). */
#include <string.h>

#include "host_check.h"
#include "cJSON.h"
#include "esp_compat_url.h"
#include "link_glue_logic.h"

static void test_product_id(void)
{
    CHECK(link_product_id_ok(NULL) && link_product_id_ok(""), "no product id: not set up, allowed");
    CHECK(link_product_id_ok("6f1c2b9e-4a7d-4e2b-9c1a-3d5e7f9a1b2c"), "a lowercase UUID");
    CHECK(!link_product_id_ok("arcs-robot-dog"), "a name is not a product id");
    CHECK(!link_product_id_ok("6F1C2B9E-4A7D-4E2B-9C1A-3D5E7F9A1B2C"), "upper case refused");
    CHECK(!link_product_id_ok("6f1c2b9e-4a7d-4e2b-9c1a-3d5e7f9a1b2"), "35 characters refused");
    CHECK(!link_product_id_ok("6f1c2b9e-4a7d-4e2b-9c1a-3d5e7f9a1b2cd"), "37 characters refused");
    CHECK(!link_product_id_ok("6f1c2b9e04a7d-4e2b-9c1a-3d5e7f9a1b2c") && !link_product_id_ok("6f1c2b9e-4a7d-4e2b-9c1a-3d5e7f9a1bzc"),
          "a misplaced dash or a non-hex digit refused");
}

static void test_backoff(void)
{
    CHECK(link_auth_refresh_interval_ms(0) == 60000 && link_auth_refresh_interval_ms(1) == 60000,
          "credential refresh: 1 min to begin with");
    CHECK(link_auth_refresh_interval_ms(2) == 120000 && link_auth_refresh_interval_ms(5) == 960000,
          "doubles per refusal");
    CHECK(link_auth_refresh_interval_ms(6) == 1800000 && link_auth_refresh_interval_ms(200) == 1800000,
          "capped at 30 min");
    CHECK(link_retry_next_ms(0) == 5000 && link_retry_next_ms(5000) == 10000 && link_retry_next_ms(40000) == 60000 &&
              link_retry_next_ms(60000) == 60000,
          "session retry 5 s doubling to 60 s");
}

static void test_result(void)
{
    cJSON *r = cJSON_Parse("{\"ok\":false,\"error\":\"\\\"effect\\\" must be a string\"}");
    link_result_to_upstream(r);
    cJSON *e = cJSON_GetObjectItem(r, "error");
    CHECK(cJSON_IsObject(e) && strcmp(cJSON_GetObjectItem(e, "code")->valuestring, "invalid_param") == 0 &&
              strcmp(cJSON_GetObjectItem(e, "message")->valuestring, "\"effect\" must be a string") == 0,
          "a refused parameter becomes {code: invalid_param, message}");
    cJSON_Delete(r);
    r = cJSON_Parse("{\"ok\":false,\"error\":\"unknown command\"}");
    link_result_to_upstream(r);
    CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "error"), "code")->valuestring, "unsupported") == 0,
          "unknown command is unsupported");
    cJSON_Delete(r);
    r = cJSON_Parse("{\"ok\":true,\"payload\":{\"a\":1}}");
    link_result_to_upstream(r);
    char *s = cJSON_PrintUnformatted(r);
    CHECK(strcmp(s, "{\"ok\":true,\"payload\":{\"a\":1}}") == 0, "a success is left alone");
    cJSON_free(s);
    cJSON_Delete(r);
}

static void test_history(void)
{
    link_history_t h;
    memset(&h, 0, sizeof(h));
    char buf[2048];
    CHECK(link_history_format(&h, buf, sizeof(buf)) == 0 && buf[0] == '\0', "no ends yet");
    for (unsigned i = 0; i < 12; i++) {
        char why[16];
        snprintf(why, sizeof(why), "why%u", i);
        link_history_add(&h, 100 + i, i, 50, i, why);
    }
    link_history_format(&h, buf, sizeof(buf));
    CHECK(strstr(buf, "why1 ") == NULL && strstr(buf, "why2 ") != NULL && strstr(buf, "why11 ") != NULL,
          "keeps the last 10");
    const char *first = "\n  ended at 102s after 2s online: why2 (last rx 50 ms before, 2 invokes so far)\n";
    CHECK(strncmp(buf, first, strlen(first)) == 0,
          "oldest first, the format `link` printed before");
    char small[40];
    int n = link_history_format(&h, small, sizeof(small));
    CHECK(n < (int)sizeof(small) && strlen(small) == (size_t)n, "a small buffer is cut, not overrun");
}

static void test_url(void)
{
    char out[256];
    CHECK(esp_compat_url_resolve("https://a.example/fetch_vms", "https://b.example/x", out, sizeof(out)) &&
              strcmp(out, "https://b.example/x") == 0,
          "absolute Location");
    CHECK(esp_compat_url_resolve("https://a.example/v1/fetch_vms?x=1", "/v2/fetch", out, sizeof(out)) &&
              strcmp(out, "https://a.example/v2/fetch") == 0,
          "absolute path keeps the origin");
    CHECK(esp_compat_url_resolve("https://a.example:8443/v1/fetch_vms", "other", out, sizeof(out)) &&
              strcmp(out, "https://a.example:8443/v1/other") == 0,
          "relative path next to the last segment");
    CHECK(esp_compat_url_resolve("https://a.example", "other", out, sizeof(out)) &&
              strcmp(out, "https://a.example/other") == 0,
          "relative to a bare origin");
    CHECK(esp_compat_url_resolve("https://a.example/p", "//c.example/q", out, sizeof(out)) &&
              strcmp(out, "https://c.example/q") == 0,
          "scheme-relative");
    CHECK(!esp_compat_url_resolve("nonsense", "/p", out, sizeof(out)), "a base that is not a URL");
    CHECK(!esp_compat_url_resolve("https://a.example/p", "/a-long-path", out, 20), "does not fit");
    CHECK(esp_compat_url_with_port("https://gadgets.impo.ai/fetch_vms", out, sizeof(out)) &&
              strcmp(out, "https://gadgets.impo.ai:443/fetch_vms") == 0,
          "get_url spells out 443");
    CHECK(esp_compat_url_with_port("https://h:8443", out, sizeof(out)) && strcmp(out, "https://h:8443/") == 0,
          "an explicit port is kept");
    char a[64], b[64];
    esp_compat_url_with_port("https://gadgets.impo.ai/fetch_vms", a, sizeof(a));
    esp_compat_url_with_port("https://gadgets.impo.ai:443/x", b, sizeof(b));
    CHECK(strncmp(a, b, strlen("https://gadgets.impo.ai:443")) == 0,
          "same origin with and without the default port (vm_api keeps Authorization)");
}

int main(void)
{
    test_product_id();
    test_backoff();
    test_result();
    test_history();
    test_url();
    DONE();
}
