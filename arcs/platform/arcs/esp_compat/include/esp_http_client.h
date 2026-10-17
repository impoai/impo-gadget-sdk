/*
 * ESP-IDF esp_http_client on ARCS: the subset vm_api.c uses (fetch_vms and the
 * device-token calls), built on platform/arcs/http_client.c. One request per
 * connection, the whole response read before the events run:
 *
 * - perform() sends the request and dispatches HTTP_EVENT_ON_DATA with the body.
 * - A 301/302/303/307/308 with disable_auto_redirect dispatches
 *   HTTP_EVENT_REDIRECT; a handler that calls esp_http_client_set_redirection()
 *   makes perform() request the Location next (at most
 *   ESP_COMPAT_HTTP_MAX_REDIRECTS times), as ESP-IDF's process_again does.
 *   Without disable_auto_redirect redirects are followed directly.
 * - get_status_code() is the last response's status, 0 when none arrived.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"   /* the SDK's (modules/heap/include/idf) */
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ESP-IDF declares it in esp_err.h; the SDK's has no names. */
const char *esp_err_to_name(esp_err_t code);

#define ESP_COMPAT_HTTP_MAX_REDIRECTS 10
#define ESP_COMPAT_HTTP_MAX_HEADERS   8
/* The most a response may hold (vm_api.c keeps at most 32 KB of body). */
#define ESP_COMPAT_HTTP_MAX_RESPONSE  (40 * 1024)

typedef struct esp_http_client *esp_http_client_handle_t;

typedef enum {
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_POST,
} esp_http_client_method_t;

typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED,
    HTTP_EVENT_REDIRECT,
} esp_http_client_event_id_t;

typedef struct esp_http_client_event {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    void *data;
    int data_len;
    void *user_data;
    char *header_key;
    char *header_value;
} esp_http_client_event_t;

typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *evt);

typedef struct {
    const char *url;
    esp_http_client_method_t method;
    int timeout_ms;          /* accepted; plat_tls reads time out after 10 s */
    int buffer_size;         /* accepted */
    int buffer_size_tx;      /* accepted */
    http_event_handle_cb event_handler;
    void *user_data;
    esp_err_t (*crt_bundle_attach)(void *conf);   /* accepted; plat_tls always verifies */
    bool disable_auto_redirect;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t client, const char *key);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int len);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
/* "scheme://host:port/path" of the current request. */
esp_err_t esp_http_client_get_url(esp_http_client_handle_t client, char *url, const int len);
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

#ifdef __cplusplus
}
#endif
