/* URL helpers of the esp_http_client compat, pure so the host tests run them. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolves a Location header against the URL it came from: absolute
 * ("https://h/p"), scheme-relative ("//h/p"), absolute path ("/p") or relative
 * ("p", next to the base's last segment). False when it does not fit or the
 * base has no "scheme://host". */
bool esp_compat_url_resolve(const char *base, const char *location, char *out, size_t cap);

/* "scheme://host:port/path..." with the port spelled out (443 for https, 80
 * for http, when the URL has none). False when it does not fit or is not a URL. */
bool esp_compat_url_with_port(const char *url, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
