#pragma once
#include <stddef.h>
#include <stdbool.h>
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

esp_err_t api_post_lock_access_log(const char *api_base,
                                  const char *device_uuid,
                                  char *bearer_token,
                                  size_t bearer_token_len,
                                  const char *device_secret,
                                  const char *card_number,
                                  const char *result,
                                  const char *direction,
                                  const char *source,
                                  const char *reason,
                                  const char *uid,
                                  const char *ts_utc,
                                  bool omit_ts,
                                  bool *out_known_member);

#ifdef __cplusplus
}
#endif
