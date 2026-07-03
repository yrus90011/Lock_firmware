#include "nvs_config.h"
#include <string.h>
#include <stdlib.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "NVS_CFG";

static esp_err_t nvs_get_str_safe(nvs_handle_t h, const char *key, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return ESP_ERR_INVALID_ARG;
    out[0] = 0;

    size_t req = 0;
    esp_err_t err = nvs_get_str(h, key, NULL, &req);
    if (err == ESP_ERR_NVS_NOT_FOUND) return err;
    if (err != ESP_OK) return err;
    if (req == 0) return ESP_ERR_NVS_INVALID_LENGTH;

    // If stored string is longer than our buffer, truncate safely
    char *tmp = (char *)malloc(req);
    if (!tmp) return ESP_ERR_NO_MEM;

    err = nvs_get_str(h, key, tmp, &req);
    if (err == ESP_OK) {
        strncpy(out, tmp, out_sz - 1);
        out[out_sz - 1] = 0;
    }
    free(tmp);
    return err;
}

static esp_err_t nvs_set_str_safe(nvs_handle_t h, const char *key, const char *val) {
    if (!val) return ESP_ERR_INVALID_ARG;
    return nvs_set_str(h, key, val);
}

esp_err_t nvs_config_load(device_config_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->last_fw_id = -1;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_DEVICE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Namespace '%s' not found yet (first boot).", NVS_NS_DEVICE);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    (void)nvs_get_str_safe(h, NVS_KEY_SSID,   out->wifi_ssid,     sizeof(out->wifi_ssid));
    (void)nvs_get_str_safe(h, NVS_KEY_PASS,   out->wifi_pass,     sizeof(out->wifi_pass));
    (void)nvs_get_str_safe(h, NVS_KEY_UUID,   out->device_uuid,   sizeof(out->device_uuid));
    (void)nvs_get_str_safe(h, NVS_KEY_SECRET, out->device_secret, sizeof(out->device_secret));
    (void)nvs_get_str_safe(h, NVS_KEY_API,    out->api_base,      sizeof(out->api_base));

    int32_t fw = -1;
    if (nvs_get_i32(h, NVS_KEY_LAST_FW, &fw) == ESP_OK) {
        out->last_fw_id = (int)fw;
    }

    size_t mac_len = sizeof(out->receiver_mac);
    if (nvs_get_blob(h, NVS_KEY_RECEIVER_MAC, out->receiver_mac, &mac_len) == ESP_OK &&
        mac_len == sizeof(out->receiver_mac)) {
        out->receiver_mac_set = true;
    }

    uint8_t bootstrap_used = 0;
    if (nvs_get_u8(h, NVS_KEY_BOOTSTRAP_USED, &bootstrap_used) == ESP_OK) {
        out->bootstrap_used = bootstrap_used != 0;
    }

    nvs_close(h);

    ESP_LOGI(TAG, "Loaded: ssid=%s uuid=%s api_base=%s last_fw_id=%d bootstrap_used=%s receiver_mac=%s%02X:%02X:%02X:%02X:%02X:%02X",
             out->wifi_ssid[0] ? out->wifi_ssid : "(missing)",
             out->device_uuid[0] ? out->device_uuid : "(missing)",
             out->api_base[0] ? out->api_base : "(missing)",
             out->last_fw_id,
             out->bootstrap_used ? "true" : "false",
             out->receiver_mac_set ? "" : "(missing) ",
             out->receiver_mac[0], out->receiver_mac[1], out->receiver_mac[2],
             out->receiver_mac[3], out->receiver_mac[4], out->receiver_mac[5]);

    return ESP_OK;
}

esp_err_t nvs_config_save(const device_config_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_DEVICE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    if (cfg->wifi_ssid[0])   ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str_safe(h, NVS_KEY_SSID, cfg->wifi_ssid));
    if (cfg->wifi_pass[0])   ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str_safe(h, NVS_KEY_PASS, cfg->wifi_pass));
    if (cfg->device_uuid[0]) ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str_safe(h, NVS_KEY_UUID, cfg->device_uuid));
    if (cfg->device_secret[0]) ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str_safe(h, NVS_KEY_SECRET, cfg->device_secret));
    if (cfg->api_base[0])    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str_safe(h, NVS_KEY_API, cfg->api_base));
    if (cfg->receiver_mac_set) ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_blob(h, NVS_KEY_RECEIVER_MAC, cfg->receiver_mac, sizeof(cfg->receiver_mac)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(h, NVS_KEY_BOOTSTRAP_USED, cfg->bootstrap_used ? 1 : 0));

    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_i32(h, NVS_KEY_LAST_FW, (int32_t)cfg->last_fw_id));

    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool nvs_config_has_wifi(const device_config_t *cfg) {
    return cfg && cfg->wifi_ssid[0] && cfg->wifi_pass[0];
}

bool nvs_config_has_identity(const device_config_t *cfg) {
    return cfg && cfg->device_uuid[0] && cfg->device_secret[0];
}

bool nvs_config_has_api_base(const device_config_t *cfg) {
    return cfg && cfg->api_base[0];
}

esp_err_t nvs_config_set_last_fw_id(int fw_id) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_DEVICE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(h, NVS_KEY_LAST_FW, (int32_t)fw_id);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_config_get_last_fw_id(int *out_fw_id) {
    if (!out_fw_id) return ESP_ERR_INVALID_ARG;
    *out_fw_id = -1;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_DEVICE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    int32_t fw = -1;
    err = nvs_get_i32(h, NVS_KEY_LAST_FW, &fw);
    nvs_close(h);
    if (err == ESP_OK) *out_fw_id = (int)fw;
    return err;
}

static bool pin_is_valid_6_digit(const char *pin) {
    if (!pin) return false;

    size_t len = strlen(pin);
    if (len != 6) return false;

    for (size_t i = 0; i < len; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            return false;
        }
    }

    return true;
}

esp_err_t nvs_config_get_pin(char *out_pin, size_t out_sz) {
    if (!out_pin || out_sz == 0) return ESP_ERR_INVALID_ARG;

    out_pin[0] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_DEVICE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_str_safe(h, NVS_KEY_PIN, out_pin, out_sz);
    nvs_close(h);

    if (err != ESP_OK) {
        return err;
    }

    if (!pin_is_valid_6_digit(out_pin)) {
        out_pin[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t nvs_config_set_pin(const char *pin) {
    if (!pin_is_valid_6_digit(pin)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_DEVICE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str_safe(h, NVS_KEY_PIN, pin);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

esp_err_t nvs_config_get_receiver_mac(uint8_t out_mac[6]) {
    if (!out_mac) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_DEVICE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = 6;
    err = nvs_get_blob(h, NVS_KEY_RECEIVER_MAC, out_mac, &len);
    nvs_close(h);

    if (err != ESP_OK) {
        return err;
    }

    return len == 6 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t nvs_config_set_receiver_mac(const uint8_t mac[6]) {
    if (!mac) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_DEVICE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(h, NVS_KEY_RECEIVER_MAC, mac, 6);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

esp_err_t nvs_config_consume_secret_cmd(uint8_t limit, uint8_t *out_used) {
    if (out_used) {
        *out_used = 0;
    }
    if (limit == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_DEVICE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t used = 0;
    err = nvs_get_u8(h, NVS_KEY_SECRET_CMD_COUNT, &used);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    if (used >= limit) {
        if (out_used) {
            *out_used = used;
        }
        nvs_close(h);
        return ESP_ERR_INVALID_STATE;
    }

    used++;
    err = nvs_set_u8(h, NVS_KEY_SECRET_CMD_COUNT, used);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (out_used) {
        *out_used = used;
    }
    return err;
}
