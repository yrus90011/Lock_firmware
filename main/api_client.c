#include "api_client.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_err.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"

typedef struct {
    char *buf;
    int len;
    int cap;
} http_resp_accum_t;

static const char *TAG = "API";

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_accum_t *acc = (http_resp_accum_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (!acc || !evt->data || evt->data_len <= 0) break;

        // For safety, ignore huge bodies
        if (acc->cap == 0) {
            acc->cap = 512;
            acc->buf = (char *)malloc(acc->cap);
            if (!acc->buf) return ESP_FAIL;
            acc->len = 0;
        }

        while (acc->len + evt->data_len + 1 > acc->cap) {
            int new_cap = acc->cap * 2;
            char *nb = (char *)realloc(acc->buf, new_cap);
            if (!nb) return ESP_FAIL;
            acc->buf = nb;
            acc->cap = new_cap;
        }

        memcpy(acc->buf + acc->len, evt->data, evt->data_len);
        acc->len += evt->data_len;
        acc->buf[acc->len] = '\0';
        break;

    default:
        break;
    }

    return ESP_OK;
}

static const char *pick_token_from_login_json(cJSON *root)
{
    if (!root) return NULL;

    // common: { "access_token": "..." }
    cJSON *tok = cJSON_GetObjectItem(root, "access_token");
    if (cJSON_IsString(tok) && tok->valuestring) return tok->valuestring;

    // common: { "token": "..." }
    tok = cJSON_GetObjectItem(root, "token");
    if (cJSON_IsString(tok) && tok->valuestring) return tok->valuestring;

    // nested: { "data": { "access_token": "..." } }
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsObject(data)) {
        tok = cJSON_GetObjectItem(data, "access_token");
        if (cJSON_IsString(tok) && tok->valuestring) return tok->valuestring;

        tok = cJSON_GetObjectItem(data, "token");
        if (cJSON_IsString(tok) && tok->valuestring) return tok->valuestring;
    }

    // sometimes: { "payload": { "access_token": "..." } }
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    if (cJSON_IsObject(payload)) {
        tok = cJSON_GetObjectItem(payload, "access_token");
        if (cJSON_IsString(tok) && tok->valuestring) return tok->valuestring;

        tok = cJSON_GetObjectItem(payload, "token");
        if (cJSON_IsString(tok) && tok->valuestring) return tok->valuestring;
    }

    return NULL;
}

esp_err_t http_read_all(esp_http_client_handle_t client, char **out_buf, int *out_len)
{
    if (!out_buf || !out_len) return ESP_ERR_INVALID_ARG;
    *out_buf = NULL;
    *out_len = 0;

    int cap = 512;
    char *buf = (char *)malloc(cap);
    if (!buf) return ESP_ERR_NO_MEM;

    int total = 0;
    while (1) {
        if (total + 256 + 1 > cap) {
            int new_cap = cap * 2;
            char *nb = (char *)realloc(buf, new_cap);
            if (!nb) { free(buf); return ESP_ERR_NO_MEM; }
            buf = nb;
            cap = new_cap;
        }

        int r = esp_http_client_read(client, buf + total, cap - total - 1);
        if (r < 0) { free(buf); return ESP_FAIL; }
        if (r == 0) break;   // done

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
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .disable_auto_redirect = true,
    };
    return esp_http_client_init(&cfg);
}

static esp_err_t sanitize_api_base_checked(char *dst, size_t dst_len, const char *src) {
  if (!dst || dst_len == 0) return ESP_ERR_INVALID_ARG;
  dst[0] = 0;
  if (!src) return ESP_ERR_INVALID_ARG;

  // skip leading whitespace
  while (*src && isspace((unsigned char)*src)) src++;

  // common bug: leading ':'
  if (*src == ':') src++;

  // copy (may truncate, but we detect "too long" below)
  size_t src_len = strlen(src);
  if (src_len >= dst_len) {
    // too long to even store
    return ESP_ERR_INVALID_SIZE;
  }
  memcpy(dst, src, src_len + 1);

  // trim trailing whitespace
  while (src_len > 0 && isspace((unsigned char)dst[src_len - 1])) {
    dst[src_len - 1] = 0;
    src_len--;
  }

  // remove trailing slashes
  while (src_len > 0 && dst[src_len - 1] == '/') {
    dst[src_len - 1] = 0;
    src_len--;
  }

  // ensure scheme exists
  if (strncmp(dst, "http://", 7) != 0 && strncmp(dst, "https://", 8) != 0) {
    // Need space for "https://" + dst + '\0'
    const size_t need = 8 + src_len + 1;
    if (need > dst_len) {
      return ESP_ERR_INVALID_SIZE;
    }

    // shift right in-place and prefix
    memmove(dst + 8, dst, src_len + 1);   // includes '\0'
    memcpy(dst, "https://", 8);
  }

  return ESP_OK;
}

static esp_err_t snprintf_checked(char *dst, size_t dst_len, const char *fmt, const char *a) {
  int n = snprintf(dst, dst_len, fmt, a);
  if (n < 0) return ESP_FAIL;
  if ((size_t)n >= dst_len) return ESP_ERR_INVALID_SIZE;
  return ESP_OK;
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

    char base[256];
    esp_err_t err = sanitize_api_base_checked(base, sizeof(base), api_base);
    if (err != ESP_OK) {
    ESP_LOGE(TAG, "api_base invalid/too long (err=%s) raw='%s'",
            esp_err_to_name(err), api_base ? api_base : "(null)");
    return err;
    }

    char url[256];
    err = snprintf_checked(url, sizeof(url), "%s/api/v1/device-auth/login", base);
    if (err != ESP_OK) {
    ESP_LOGE(TAG, "login URL too long (err=%s) base='%s'",
            esp_err_to_name(err), base);
    return err;
    }

    ESP_LOGW("OTA", "api_base_sanitized='%s'", base);
    ESP_LOGW("OTA", "Login URL='%s'", url);

    char body[512];
    int n = snprintf(body, sizeof(body),
                     "{\"device_uuid\":\"%s\",\"device_secret\":\"%s\"}",
                     device_uuid, device_secret);
    if (n <= 0 || n >= (int)sizeof(body)) return ESP_ERR_INVALID_SIZE;

    http_resp_accum_t acc = {0};

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .event_handler = http_event_handler,
        .user_data = &acc,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Connection", "close");
    esp_http_client_set_post_field(client, body, strlen(body));

    err = esp_http_client_perform(client);

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "login status=%d", status);

    // cleanup client *after* perform is done
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "login perform failed: %s", esp_err_to_name(err));
        if (acc.buf) free(acc.buf);
        return err;
    }

    if (status != 200) {
        if (acc.buf) ESP_LOGW(TAG, "login body: %s", acc.buf);
        if (acc.buf) free(acc.buf);
        return ESP_FAIL;
    }

    if (!acc.buf || acc.len <= 0) {
        ESP_LOGE(TAG, "login: empty body (status 200)");
        if (acc.buf) free(acc.buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "login resp_len=%d", acc.len);
    ESP_LOGI(TAG, "login body (head): %.*s", acc.len > 300 ? 300 : acc.len, acc.buf);

    // ---- IMPORTANT: copy into a fresh buffer before parsing ----
    char *json = (char *)malloc(acc.len + 1);
    if (!json) {
        free(acc.buf);
        return ESP_ERR_NO_MEM;
    }
    memcpy(json, acc.buf, acc.len + 1);  // includes '\0'
    free(acc.buf);
    acc.buf = NULL;

    cJSON *root = cJSON_Parse(json);
    free(json);

    if (!root) {
        ESP_LOGE(TAG, "login: invalid JSON");
        return ESP_FAIL;
    }

    cJSON *tok = cJSON_GetObjectItem(root, "access_token");
    if (!cJSON_IsString(tok) || !tok->valuestring) {
        ESP_LOGE(TAG, "login: token missing");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t tlen = strlen(tok->valuestring);
    if (tlen + 1 > out_token_len) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out_token, tok->valuestring, tlen + 1);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "login OK token_len=%u", (unsigned)tlen);
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
    ESP_LOGI(TAG, "login resp_len=%d body=%.*s",
         resp_len, resp_len > 200 ? 200 : resp_len, resp ? resp : "");

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

esp_err_t api_get_firmware_list_json(const char *api_base,
                                    const char *device_uuid,
                                    const char *bearer_token,
                                    char **out_json,
                                    int *out_len)
{
    if (!api_base || !device_uuid || !bearer_token || !out_json || !out_len) return ESP_ERR_INVALID_ARG;
    *out_json = NULL;
    *out_len = 0;

    char url[320];
    snprintf(url, sizeof(url), "%s/api/v1/devices/%s/firmware", api_base, device_uuid);

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
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    char *resp = NULL;
    int resp_len = 0;
    err = http_read_all(client, &resp, &resp_len);
    ESP_LOGI(TAG, "login resp_len=%d body=%.*s",
         resp_len, resp_len > 200 ? 200 : resp_len, resp ? resp : "");

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) { free(resp); return err; }
    if (status != 200 || !resp) { free(resp); return ESP_FAIL; }

    // resp is malloc'd by http_read_all; return ownership to caller
    *out_json = resp;
    *out_len = resp_len;
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
    ESP_LOGI(TAG, "login resp_len=%d body=%.*s",
         resp_len, resp_len > 200 ? 200 : resp_len, resp ? resp : "");

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