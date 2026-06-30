// ws_client.h
#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ws_event_cb_t)(const char *type, const char *payload_command);

// NEW: called when WS handshake fails due to auth (401/403) and we need a new token
typedef void (*ws_relogin_cb_t)(void);

esp_err_t ws_client_start(const char *api_base,
                          const char *bearer_token,
                          ws_event_cb_t cb);

void ws_client_stop(void);

// device -> server send helper (thread-safe enough)
esp_err_t ws_client_send_text(const char *text);
esp_err_t ws_send_text_locked(const char *text);

bool ws_client_is_connected(void);
void stop_backend(void);
void start_backend(void);
// NEW
void ws_client_set_relogin_cb(ws_relogin_cb_t cb);

#ifdef __cplusplus
}
#endif