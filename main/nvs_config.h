#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
#define NVS_KEY_PIN "pin"
#define NVS_KEY_RECEIVER_MAC "receiver_mac"
#define NVS_KEY_BOOTSTRAP_USED "bootstrap_used"

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char device_uuid[64];     // UUID string
    char device_secret[128];  // allow longer secrets
    char api_base[192];       // "https://host"
    uint8_t receiver_mac[6];  // ESP-NOW receiver peer
    bool receiver_mac_set;
    bool bootstrap_used;
    int  last_fw_id;          // -1 if none
} device_config_t;

esp_err_t nvs_config_load(device_config_t *out);
esp_err_t nvs_config_save(const device_config_t *cfg);

bool nvs_config_has_wifi(const device_config_t *cfg);
bool nvs_config_has_identity(const device_config_t *cfg);
bool nvs_config_has_api_base(const device_config_t *cfg);

esp_err_t nvs_config_set_last_fw_id(int fw_id);
esp_err_t nvs_config_get_last_fw_id(int *out_fw_id);

esp_err_t nvs_config_get_pin(char *out_pin, size_t out_sz);
esp_err_t nvs_config_set_pin(const char *pin);

esp_err_t nvs_config_get_receiver_mac(uint8_t out_mac[6]);
esp_err_t nvs_config_set_receiver_mac(const uint8_t mac[6]);


#ifdef __cplusplus
}
#endif
