/* URL helpers of the esp_http_client compat (esp_compat_url.h). */
#include <stdio.h>
#include <string.h>

#include "esp_compat_url.h"

/* Length of "scheme://host[:port]" at the start of url, 0 when it is not a URL. */
static size_t origin_len(const char *url)
{
    const char *p = strstr(url, "://");
    if (p == NULL || p == url) {
        return 0;
    }
    p += 3;
    size_t host = strcspn(p, "/?#");
    if (host == 0) {
        return 0;
    }
    return (size_t)(p - url) + host;
}

static bool put(char *out, size_t cap, const char *a, size_t alen, const char *b)
{
    int n = snprintf(out, cap, "%.*s%s", (int)alen, a, b);
    return n >= 0 && (size_t)n < cap;
}

bool esp_compat_url_resolve(const char *base, const char *location, char *out, size_t cap)
{
    if (base == NULL || location == NULL || out == NULL || cap == 0 || location[0] == '\0') {
        return false;
    }
    if (origin_len(location) > 0) {
        return put(out, cap, "", 0, location);
    }
    size_t origin = origin_len(base);
    if (origin == 0) {
        return false;
    }
    if (strncmp(location, "//", 2) == 0) {
        const char *colon = strchr(base, ':');
        return put(out, cap, base, (size_t)(colon - base) + 1, location);
    }
    if (location[0] == '/') {
        return put(out, cap, base, origin, location);
    }
    /* Relative: replace what follows the base path's last '/'. */
    size_t path_end = origin + strcspn(base + origin, "?#");
    size_t dir = origin;
    for (size_t i = origin; i < path_end; i++) {
        if (base[i] == '/') {
            dir = i + 1;
        }
    }
    if (dir == origin) {
        int n = snprintf(out, cap, "%.*s/%s", (int)origin, base, location);
        return n >= 0 && (size_t)n < cap;
    }
    return put(out, cap, base, dir, location);
}

bool esp_compat_url_with_port(const char *url, char *out, size_t cap)
{
    if (url == NULL || out == NULL || cap == 0) {
        return false;
    }
    size_t origin = origin_len(url);
    if (origin == 0) {
        return false;
    }
    const char *host = strstr(url, "://") + 3;
    const char *rest = url + origin;
    const char *path = rest[0] != '\0' ? rest : "/";
    if (memchr(host, ':', (size_t)(rest - host)) != NULL) {
        int n = snprintf(out, cap, "%.*s%s", (int)origin, url, path);
        return n >= 0 && (size_t)n < cap;
    }
    const char *port = strncmp(url, "https://", 8) == 0 ? "443" : "80";
    int n = snprintf(out, cap, "%.*s:%s%s", (int)origin, url, port, path);
    return n >= 0 && (size_t)n < cap;
}
