/*
 * Minimal HTTPS/1.1 client: one request per connection (Connection: close),
 * fixed-length and chunked bodies. Enough for fetch_vms and token refresh.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform_arcs.h"
#include "http_client.h"

#define TAG "http"
#include "lisa_log.h"

#define READ_TIMEOUT_MS 10000

int http_parse_url(const char *url, http_url_t *out)
{
    memset(out, 0, sizeof(*out));
    if (strncmp(url, "https://", 8) != 0) {
        return -1;
    }
    const char *host = url + 8;
    const char *path = strchr(host, '/');
    const char *host_end = path ? path : host + strlen(host);
    const char *colon = memchr(host, ':', (size_t)(host_end - host));
    const char *name_end = colon ? colon : host_end;
    size_t n = (size_t)(name_end - host);
    if (n == 0 || n >= sizeof(out->host)) {
        return -1;
    }
    memcpy(out->host, host, n);
    out->port = colon ? (uint16_t)atoi(colon + 1) : 443;
    snprintf(out->path, sizeof(out->path), "%s", path ? path : "/");
    return 0;
}

/* Decode a chunked body in place; returns the decoded length or -1. */
static int dechunk(char *body, size_t len)
{
    size_t in = 0, out = 0;
    while (in < len) {
        char *end;
        unsigned long size = strtoul(body + in, &end, 16);
        char *crlf = strstr(end, "\r\n");
        if (crlf == NULL) {
            return -1;
        }
        in = (size_t)(crlf + 2 - body);
        if (size == 0) {
            break;
        }
        if (in + size > len) {
            return -1;
        }
        memmove(body + out, body + in, size);
        out += size;
        in += size + 2;
    }
    body[out] = '\0';
    return (int)out;
}

static const char *find_header(const char *head, const char *name)
{
    size_t n = strlen(name);
    for (const char *line = strstr(head, "\r\n"); line != NULL; line = strstr(line, "\r\n")) {
        line += 2;
        if (strncasecmp(line, name, n) == 0 && line[n] == ':') {
            const char *v = line + n + 1;
            while (*v == ' ') {
                v++;
            }
            return v;
        }
    }
    return NULL;
}

int http_request(const char *method, const char *url, const char *extra_headers,
                 const void *body, size_t body_len, size_t max_response, http_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));
    http_url_t u;
    if (http_parse_url(url, &u) != 0) {
        LISA_LOGE(TAG, "bad url %s", url);
        return -1;
    }

    int err = 0;
    plat_tls_t *tls = plat_tls_connect(u.host, u.port, READ_TIMEOUT_MS, &err);
    if (tls == NULL) {
        return err ? err : -1;
    }

    /* Bearer tokens can be a few KB. */
    size_t head_cap = 512 + strlen(u.path) + (extra_headers ? strlen(extra_headers) : 0);
    char *head = malloc(head_cap);
    if (head == NULL) {
        plat_tls_close(tls);
        return -1;
    }
    int n = snprintf(head, head_cap,
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: impo-gadget-arcs/%s\r\n"
                     "Connection: close\r\n"
                     "Content-Length: %u\r\n"
                     "%s"
                     "\r\n",
                     method, u.path, u.host, plat_firmware_version(), (unsigned)body_len,
                     extra_headers ? extra_headers : "");
    int ret = -1;
    if (n <= 0 || (size_t)n >= head_cap) {
        LISA_LOGE(TAG, "request head too long");
        goto out;
    }
    if (plat_tls_write(tls, head, (size_t)n) < 0 ||
        (body_len > 0 && plat_tls_write(tls, body, body_len) < 0)) {
        goto out;
    }
    LISA_LOGI(TAG, "sent %s %s (%d header bytes)", method, u.path, n);

    char *buf = malloc(max_response + 1);
    if (buf == NULL) {
        goto out;
    }
    size_t got = 0;
    for (;;) {
        if (got == max_response) {
            LISA_LOGW(TAG, "response truncated at %u bytes", (unsigned)max_response);
            resp->truncated = true;
            break;
        }
        int r = plat_tls_read(tls, buf + got, max_response - got, READ_TIMEOUT_MS);
        LISA_LOGD(TAG, "read %d (total %u)", r, (unsigned)(got + (r > 0 ? r : 0)));
        if (r > 0) {
            got += (size_t)r;
            continue;
        }
        if (r < 0 && got == 0) {
            free(buf);
            ret = r;
            goto out;
        }
        break; /* closed (or error after data): parse what arrived */
    }
    buf[got] = '\0';

    char *sep = strstr(buf, "\r\n\r\n");
    if (sep == NULL || sscanf(buf, "HTTP/1.%*d %d", &resp->status) != 1) {
        LISA_LOGE(TAG, "malformed response (%u bytes)", (unsigned)got);
        free(buf);
        goto out;
    }
    *sep = '\0';
    const char *loc = find_header(buf, "Location");
    if (loc != NULL) {
        size_t n = strcspn(loc, "\r\n");
        if (n < sizeof(resp->location)) {
            memcpy(resp->location, loc, n);
            resp->location[n] = '\0';
        }
    }
    char *payload = sep + 4;
    size_t payload_len = got - (size_t)(payload - buf);
    const char *te = find_header(buf, "Transfer-Encoding");
    if (te != NULL && strncasecmp(te, "chunked", 7) == 0) {
        int d = dechunk(payload, payload_len);
        if (d < 0) {
            LISA_LOGE(TAG, "bad chunked body");
            free(buf);
            goto out;
        }
        payload_len = (size_t)d;
    }
    memmove(buf, payload, payload_len + 1);
    resp->body = buf;
    resp->body_len = payload_len;
    ret = 0;

out:
    free(head);
    plat_tls_close(tls);
    return ret;
}

void http_response_free(http_response_t *resp)
{
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
}
