#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*access_log_ready_cb_t)(void);
typedef void (*access_log_start_backend_cb_t)(void);

esp_err_t access_log_service_start(access_log_ready_cb_t ready_cb,
                                   access_log_start_backend_cb_t start_backend_cb);

void access_log_service_set_context(const char *api_base,
                                    const char *device_uuid,
                                    const char *device_secret,
                                    const char *token);

esp_err_t access_log_service_enqueue(const char *uid,
                                     const char *result,
                                     const char *reason,
                                     bool omit_ts);

#ifdef __cplusplus
}
#endif
