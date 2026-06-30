#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_client.h" 

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  id;
    char sha256_hex[65];   // 64 + NUL
    int  file_size;
    char version[32];      // optional (from list)
} firmware_info_t;

esp_err_t http_read_all(esp_http_client_handle_t client, char **out_buf, int *out_len);

esp_err_t api_device_login(const char *api_base,
                           const char *device_uuid,
                           const char *device_secret,
                           char *out_token,
                           size_t out_token_len);

esp_err_t api_get_latest_firmware(const char *api_base,
                                  const char *device_uuid,
                                  const char *bearer_token,
                                  firmware_info_t *out_fw);

esp_err_t api_get_firmware_list_json(const char *api_base,
                                    const char *device_uuid,
                                    const char *bearer_token,
                                    char **out_json,
                                    int *out_len);
                                  

esp_err_t api_get_firmware_by_id(const char *api_base,
                                 const char *device_uuid,
                                 const char *bearer_token,
                                 int fw_id,
                                 firmware_info_t *out_fw);

#ifdef __cplusplus
}
#endif