/* Minimal HTTPS client used by the link layer (L1). */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char host[128];
    uint16_t port;
    char path[384];
} http_url_t;

typedef struct {
    int status;
    char *body;        /* NUL-terminated, heap; free with http_response_free */
    size_t body_len;
    bool truncated;    /* the body was longer than max_response */
    char location[512];   /* Location header of a redirect, else "" */
} http_response_t;

int http_parse_url(const char *url, http_url_t *out);

/* extra_headers: zero or more "Name: value\r\n" lines. Returns 0 when a response was parsed. */
int http_request(const char *method, const char *url, const char *extra_headers,
                 const void *body, size_t body_len, size_t max_response, http_response_t *resp);

void http_response_free(http_response_t *resp);

#ifdef __cplusplus
}
#endif
