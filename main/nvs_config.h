#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_NS_DEVICE   "ESPLOCK1"

#define NVS_KEY_SSID    "wifi_ssid"
#define NVS_KEY_PASS    "wifi_pass"
#define NVS_KEY_UUID    "device_uuid"
#define NVS_KEY_SECRET  "device_secret"
#define NVS_KEY_API     "api_base"
#define NVS_KEY_LAST_FW "last_fw_id"

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char device_uuid[64];     // UUID string
    char device_secret[128];  // allow longer secrets
    char api_base[192];       // "https://host"
    int  last_fw_id;          // -1 if none
} device_config_t;

esp_err_t nvs_config_load(device_config_t *out);
esp_err_t nvs_config_save(const device_config_t *cfg);

bool nvs_config_has_wifi(const device_config_t *cfg);
bool nvs_config_has_identity(const device_config_t *cfg);
bool nvs_config_has_api_base(const device_config_t *cfg);

esp_err_t nvs_config_set_last_fw_id(int fw_id);
esp_err_t nvs_config_get_last_fw_id(int *out_fw_id);

#ifdef __cplusplus
}
#endif