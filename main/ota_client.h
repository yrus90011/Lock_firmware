#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

bool ota_client_is_running(void);

void ota_mark_valid_if_pending_verify(void);

esp_err_t ota_client_start(const char *api_base,
                           const char *device_uuid,
                           const char *bearer_token);

esp_err_t ota_client_start_by_id(const char *api_base,
                                 const char *device_uuid,
                                 const char *bearer_token,
                                 int fw_id);

#ifdef __cplusplus
}
#endif