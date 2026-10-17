/* ESP-IDF esp_http_client on platform/arcs/http_client.c; see include/esp_http_client.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "http_client.h"
#include "esp_compat_url.h"
#include "esp_http_client.h"

#define TAG "esp_http"
#include "lisa_log.h"

struct esp_http_client {
    char url[512];
    esp_http_client_method_t method;
    http_event_handle_cb handler;
    void *user_data;
    bool auto_redirect;
    char *keys[ESP_COMPAT_HTTP_MAX_HEADERS];
    char *values[ESP_COMPAT_HTTP_MAX_HEADERS];
    const char *post;
    int post_len;
    int status;
    char location[512];
    bool process_again;   /* set_redirection() asked for the Location next */
    int redirects;
};

static esp_err_t dispatch(esp_http_client_handle_t c, esp_http_client_event_id_t id, void *data, int len)
{
    if (c->handler == NULL) {
        return ESP_OK;
    }
    esp_http_client_event_t evt = { .event_id = id, .client = c, .data = data, .data_len = len,
                                    .user_data = c->user_data };
    return c->handler(&evt);
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    if (config == NULL || config->url == NULL) {
        return NULL;
    }
    struct esp_http_client *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    if ((size_t)snprintf(c->url, sizeof(c->url), "%s", config->url) >= sizeof(c->url)) {
        free(c);
        return NULL;
    }
    c->method = config->method;
    c->handler = config->event_handler;
    c->user_data = config->user_data;
    c->auto_redirect = !config->disable_auto_redirect;
    return c;
}

static int find_header(esp_http_client_handle_t c, const char *key)
{
    for (int i = 0; i < ESP_COMPAT_HTTP_MAX_HEADERS; i++) {
        if (c->keys[i] != NULL && strcasecmp(c->keys[i], key) == 0) {
            return i;
        }
    }
    return -1;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value)
{
    if (c == NULL || key == NULL || value == NULL || strpbrk(key, "\r\n:") || strpbrk(value, "\r\n")) {
        return ESP_ERR_INVALID_ARG;
    }
    int i = find_header(c, key);
    for (int k = 0; i < 0 && k < ESP_COMPAT_HTTP_MAX_HEADERS; k++) {
        if (c->keys[k] == NULL) {
            i = k;
        }
    }
    if (i < 0) {
        return ESP_ERR_NO_MEM;
    }
    char *v = strdup(value);
    char *k = c->keys[i] != NULL ? c->keys[i] : strdup(key);
    if (v == NULL || k == NULL) {
        free(v);
        if (k != c->keys[i]) {
            free(k);
        }
        return ESP_ERR_NO_MEM;
    }
    free(c->values[i]);
    c->keys[i] = k;
    c->values[i] = v;
    return ESP_OK;
}

esp_err_t esp_http_client_delete_header(esp_http_client_handle_t c, const char *key)
{
    if (c == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int i = find_header(c, key);
    if (i >= 0) {
        free(c->keys[i]);
        free(c->values[i]);
        c->keys[i] = c->values[i] = NULL;
    }
    return ESP_OK;
}

esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *data, int len)
{
    if (c == NULL || len < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    c->post = data;
    c->post_len = data != NULL ? len : 0;
    return ESP_OK;
}

static char *header_lines(esp_http_client_handle_t c)
{
    size_t len = 1;
    for (int i = 0; i < ESP_COMPAT_HTTP_MAX_HEADERS; i++) {
        if (c->keys[i] != NULL) {
            len += strlen(c->keys[i]) + strlen(c->values[i]) + 4;
        }
    }
    char *out = malloc(len);
    if (out == NULL) {
        return NULL;
    }
    size_t w = 0;
    out[0] = '\0';
    for (int i = 0; i < ESP_COMPAT_HTTP_MAX_HEADERS; i++) {
        if (c->keys[i] != NULL) {
            w += (size_t)snprintf(out + w, len - w, "%s: %s\r\n", c->keys[i], c->values[i]);
        }
    }
    return out;
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t c)
{
    if (c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (;;) {
        c->status = 0;
        c->location[0] = '\0';
        c->process_again = false;
        char *headers = header_lines(c);
        if (headers == NULL) {
            return ESP_ERR_NO_MEM;
        }
        http_response_t resp;
        int r = http_request(c->method == HTTP_METHOD_POST ? "POST" : "GET", c->url, headers, c->post,
                             (size_t)c->post_len, ESP_COMPAT_HTTP_MAX_RESPONSE, &resp);
        free(headers);
        if (r != 0) {
            dispatch(c, HTTP_EVENT_ERROR, NULL, 0);
            return ESP_FAIL;
        }
        c->status = resp.status;
        snprintf(c->location, sizeof(c->location), "%s", resp.location);
        if (is_redirect(resp.status) && c->location[0] != '\0') {
            if (c->redirects >= ESP_COMPAT_HTTP_MAX_REDIRECTS) {
                LISA_LOGW(TAG, "more than %d redirects", ESP_COMPAT_HTTP_MAX_REDIRECTS);
                http_response_free(&resp);
                return ESP_FAIL;
            }
            if (c->auto_redirect) {
                esp_http_client_set_redirection(c);
            } else {
                dispatch(c, HTTP_EVENT_REDIRECT, NULL, 0);
            }
            if (c->process_again) {
                http_response_free(&resp);
                continue;
            }
        }
        if (resp.body_len > 0) {
            dispatch(c, HTTP_EVENT_ON_DATA, resp.body, (int)resp.body_len);
        }
        dispatch(c, HTTP_EVENT_ON_FINISH, NULL, 0);
        http_response_free(&resp);
        return ESP_OK;
    }
}

int esp_http_client_get_status_code(esp_http_client_handle_t c)
{
    return c != NULL ? c->status : 0;
}

esp_err_t esp_http_client_get_url(esp_http_client_handle_t c, char *url, const int len)
{
    if (c == NULL || url == NULL || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_compat_url_with_port(c->url, url, (size_t)len) ? ESP_OK : ESP_FAIL;
}

esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t c)
{
    if (c == NULL || c->location[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char next[sizeof(c->url)];
    if (!esp_compat_url_resolve(c->url, c->location, next, sizeof(next))) {
        return ESP_FAIL;
    }
    memcpy(c->url, next, sizeof(next));
    c->redirects++;
    c->process_again = true;
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    if (c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < ESP_COMPAT_HTTP_MAX_HEADERS; i++) {
        free(c->keys[i]);
        free(c->values[i]);
    }
    free(c);
    return ESP_OK;
}
