#include "api_client.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "API";

esp_err_t http_read_all(esp_http_client_handle_t client, char **out_buf, int *out_len)
{
    if (!out_buf || !out_len) return ESP_ERR_INVALID_ARG;
    *out_buf = NULL;
    *out_len = 0;

    const int CHUNK = 512;
    int cap = 2048;
    int total = 0;

    char *buf = (char *)malloc(cap);
    if (!buf) return ESP_ERR_NO_MEM;

    while (1) {
        char tmp[CHUNK];
        int r = esp_http_client_read(client, tmp, CHUNK);
        if (r < 0) { free(buf); return ESP_FAIL; }

        if (r == 0) {
            if (esp_http_client_is_complete_data_received(client)) break;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (total + r + 1 > cap) {
            int new_cap = cap * 2;
            while (new_cap < total + r + 1) new_cap *= 2;
            char *nb = (char *)realloc(buf, new_cap);
            if (!nb) { free(buf); return ESP_ERR_NO_MEM; }
            buf = nb;
            cap = new_cap;
        }

        memcpy(buf + total, tmp, r);
        total += r;
    }

    buf[total] = '\0';
    *out_buf = buf;
    *out_len = total;
    return ESP_OK;
}

static esp_http_client_handle_t make_client(const char *url, int timeout_ms)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };
    return esp_http_client_init(&cfg);
}

esp_err_t api_device_login(const char *api_base,
                           const char *device_uuid,
                           const char *device_secret,
                           char *out_token,
                           size_t out_token_len)
{
    if (!api_base || !device_uuid || !device_secret || !out_token || out_token_len < 16) {
        return ESP_ERR_INVALID_ARG;
    }
    out_token[0] = 0;

    char url[256];
    ESP_LOGW("OTA", "api_base='%s'", api_base ? api_base : "(null)");
    snprintf(url, sizeof(url), "%s/api/v1/device-auth/login", api_base);
    ESP_LOGW("OTA", "Download URL = '%s'", url);


    char body[512];
    snprintf(body, sizeof(body),
             "{\"device_uuid\":\"%s\",\"device_secret\":\"%s\"}",
             device_uuid, device_secret);

    esp_http_client_handle_t client = make_client(url, 20000);
    if (!client) return ESP_FAIL;

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = esp_http_client_open(client, (int)strlen(body));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "login open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int w = esp_http_client_write(client, body, (int)strlen(body));
    if (w < 0) {
        ESP_LOGE(TAG, "login write failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "login status=%d", status);

    char *resp = NULL;
    int resp_len = 0;
    err = http_read_all(client, &resp, &resp_len);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "login read failed: %s", esp_err_to_name(err));
        free(resp);
        return err;
    }

    ESP_LOGI(TAG, "login body_len=%d", resp_len);

    if (status != 200 || !resp) {
        if (resp) ESP_LOGW(TAG, "login body: %s", resp);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) return ESP_FAIL;

    cJSON *tok = cJSON_GetObjectItem(root, "access_token");
    if (!cJSON_IsString(tok) || !tok->valuestring) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strncpy(out_token, tok->valuestring, out_token_len - 1);
    out_token[out_token_len - 1] = 0;

    cJSON_Delete(root);
    ESP_LOGI(TAG, "login OK token_len=%u", (unsigned)strlen(out_token));
    return ESP_OK;
}

esp_err_t api_get_latest_firmware(const char *api_base,
                                  const char *device_uuid,
                                  const char *bearer_token,
                                  firmware_info_t *out_fw)
{
    if (!api_base || !device_uuid || !bearer_token || !out_fw) return ESP_ERR_INVALID_ARG;
    memset(out_fw, 0, sizeof(*out_fw));
    out_fw->id = -1;

    char url[320];
    ESP_LOGW("OTA", "api_base='%s'", api_base ? api_base : "(null)");
    snprintf(url, sizeof(url), "%s/api/v1/devices/%s/firmware", api_base, device_uuid);
    ESP_LOGW("OTA", "Download URL = '%s'", url);

    esp_http_client_handle_t client = make_client(url, 20000);
    if (!client) return ESP_FAIL;

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "accept", "application/json");
    esp_http_client_set_header(client, "Connection", "close");

    char auth[600];
    snprintf(auth, sizeof(auth), "Bearer %s", bearer_token);
    esp_http_client_set_header(client, "Authorization", auth);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fwlist open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "fwlist status=%d", status);

    char *resp = NULL;
    int resp_len = 0;
    err = http_read_all(client, &resp, &resp_len);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) { free(resp); return err; }
    ESP_LOGI(TAG, "fwlist body_len=%d", resp_len);

    if (status != 200 || !resp) {
        if (resp) ESP_LOGW(TAG, "fwlist body: %s", resp);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *arr = cJSON_Parse(resp);
    free(resp);
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return ESP_FAIL; }

    int n = cJSON_GetArraySize(arr);
    if (n <= 0) { cJSON_Delete(arr); return ESP_ERR_NOT_FOUND; }

    // newest-first assumed; take index 0
    cJSON *it = cJSON_GetArrayItem(arr, 0);
    cJSON *jid  = cJSON_GetObjectItem(it, "id");
    cJSON *jsha = cJSON_GetObjectItem(it, "sha256");
    cJSON *jfs  = cJSON_GetObjectItem(it, "file_size");
    cJSON *jver = cJSON_GetObjectItem(it, "version");

    if (!cJSON_IsNumber(jid) || !cJSON_IsString(jsha) || !jsha->valuestring || !cJSON_IsNumber(jfs)) {
        cJSON_Delete(arr);
        return ESP_FAIL;
    }

    out_fw->id = jid->valueint;
    out_fw->file_size = jfs->valueint;
    strncpy(out_fw->sha256_hex, jsha->valuestring, sizeof(out_fw->sha256_hex) - 1);
    out_fw->sha256_hex[64] = 0;

    if (cJSON_IsString(jver) && jver->valuestring) {
        strncpy(out_fw->version, jver->valuestring, sizeof(out_fw->version) - 1);
    }

    ESP_LOGI(TAG, "latest fw: id=%d size=%d sha256=%s ver=%s",
             out_fw->id, out_fw->file_size, out_fw->sha256_hex,
             out_fw->version[0] ? out_fw->version : "-");

    cJSON_Delete(arr);
    return ESP_OK;
}

esp_err_t api_get_firmware_by_id(const char *api_base,
                                 const char *device_uuid,
                                 const char *bearer_token,
                                 int fw_id,
                                 firmware_info_t *out_fw)
{
    if (!api_base || !device_uuid || !bearer_token || !out_fw || fw_id <= 0) return ESP_ERR_INVALID_ARG;
    memset(out_fw, 0, sizeof(*out_fw));
    out_fw->id = -1;

    char url[320];
    ESP_LOGW("OTA", "api_base='%s'", api_base ? api_base : "(null)");
    snprintf(url, sizeof(url), "%s/api/v1/devices/%s/firmware", api_base, device_uuid);
    ESP_LOGW("OTA", "Download URL = '%s'", url);

    esp_http_client_handle_t client = make_client(url, 20000);
    if (!client) return ESP_FAIL;

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "accept", "application/json");
    esp_http_client_set_header(client, "Connection", "close");

    char auth[600];
    snprintf(auth, sizeof(auth), "Bearer %s", bearer_token);
    esp_http_client_set_header(client, "Authorization", auth);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fwlist open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    char *resp = NULL;
    int resp_len = 0;
    err = http_read_all(client, &resp, &resp_len);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) { free(resp); return err; }
    if (status != 200 || !resp) { free(resp); return ESP_FAIL; }

    cJSON *arr = cJSON_Parse(resp);
    free(resp);

    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return ESP_FAIL;
    }

    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        cJSON *jid  = cJSON_GetObjectItem(it, "id");
        cJSON *jsha = cJSON_GetObjectItem(it, "sha256");
        cJSON *jfs  = cJSON_GetObjectItem(it, "file_size");
        cJSON *jver = cJSON_GetObjectItem(it, "version");

        if (cJSON_IsNumber(jid) && jid->valueint == fw_id &&
            cJSON_IsString(jsha) && jsha->valuestring &&
            cJSON_IsNumber(jfs))
        {
            out_fw->id = fw_id;
            out_fw->file_size = jfs->valueint;

            strncpy(out_fw->sha256_hex, jsha->valuestring, 64);
            out_fw->sha256_hex[64] = 0;

            if (cJSON_IsString(jver) && jver->valuestring) {
                strncpy(out_fw->version, jver->valuestring, sizeof(out_fw->version) - 1);
            }

            cJSON_Delete(arr);
            return ESP_OK;
        }
    }

    cJSON_Delete(arr);
    return ESP_ERR_NOT_FOUND;
}