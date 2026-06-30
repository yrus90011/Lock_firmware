// ws_client.h
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ws_event_cb_t)(const char *type, const char *payload_command);

esp_err_t ws_client_start(const char *api_base,
                          const char *bearer_token,
                          ws_event_cb_t cb);

void ws_client_stop(void);

// device -> server send helper (thread-safe enough)
esp_err_t ws_client_send_text(const char *text);

// allow OTA flow to stop auxiliary WS tasks (keepalive)
void ws_client_suspend_aux_tasks(void);
void ws_client_resume_aux_tasks(void);

#ifdef __cplusplus
}
#endif