#include "ws_client.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "cJSON.h"

static const char *TAG = "WS";

static esp_websocket_client_handle_t s_ws = NULL;
static TaskHandle_t s_keepalive_task = NULL;
static SemaphoreHandle_t s_ws_tx_mu = NULL;
static ws_event_cb_t s_cb = NULL;

// store device_uuid if you later want to send a hello; not required for now
static char s_api_base[192] = {0};

static void build_ws_url_from_api_base(const char *api_base, char *out, size_t out_sz)
{
    // api_base is "https://host" -> ws becomes "wss://host/api/v1/ws/device"
    const char *p = api_base;
    if (!strncmp(p, "https://", 8)) p += 8;
    else if (!strncmp(p, "http://", 7)) p += 7;

    // strip trailing slashes
    char host[160] = {0};
    strncpy(host, p, sizeof(host) - 1);
    size_t n = strlen(host);
    while (n && host[n - 1] == '/') { host[n - 1] = 0; n--; }

    snprintf(out, out_sz, "wss://%s/api/v1/ws/device", host);
}

static void log_hex_first64(const uint8_t *data, int len)
{
    const int max = (len > 64) ? 64 : len;
    char line[3 * 64 + 1];
    int pos = 0;
    for (int i = 0; i < max; i++) {
        pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", data[i]);
        if (pos >= (int)sizeof(line)) break;
    }
    line[sizeof(line) - 1] = 0;
    ESP_LOGI(TAG, "BIN(%d) HEX(first %d): %s%s", len, max, line, (len > max) ? "..." : "");
}

static esp_err_t ws_send_text_locked(const char *text)
{
    if (!text) return ESP_ERR_INVALID_ARG;
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return ESP_ERR_INVALID_STATE;

    if (!s_ws_tx_mu) {
        s_ws_tx_mu = xSemaphoreCreateMutex();
        if (!s_ws_tx_mu) return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_ws_tx_mu, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    int rc = esp_websocket_client_send_text(s_ws, text, (int)strlen(text), pdMS_TO_TICKS(3000));

    xSemaphoreGive(s_ws_tx_mu);
    return (rc < 0) ? ESP_FAIL : ESP_OK;
}

esp_err_t ws_client_send_text(const char *text)
{
    esp_err_t err = ws_send_text_locked(text);
    if (err == ESP_OK) ESP_LOGI(TAG, "TX: %s", text);
    else ESP_LOGW(TAG, "TX failed: %s", esp_err_to_name(err));
    return err;
}

static void ws_keepalive_task_fn(void *arg)
{
    (void)arg;
    while (1) {
        if (s_ws && esp_websocket_client_is_connected(s_ws)) {
            (void)ws_send_text_locked("{\"type\":\"status_check\"}");
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void ws_client_suspend_aux_tasks(void)
{
    if (s_keepalive_task) vTaskSuspend(s_keepalive_task);
}

void ws_client_resume_aux_tasks(void)
{
    if (s_keepalive_task) vTaskResume(s_keepalive_task);
}

static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "CONNECTED");
            // send initial status_check
            (void)ws_send_text_locked("{\"type\":\"status_check\"}");
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "DISCONNECTED");
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "ERROR");
            break;

        case WEBSOCKET_EVENT_DATA: {
            if (d->op_code == 0x1 /* text */) {
                // not null-terminated
                char *msg = (char *)malloc(d->data_len + 1);
                if (!msg) break;
                memcpy(msg, d->data_ptr, d->data_len);
                msg[d->data_len] = 0;

                ESP_LOGI(TAG, "RX: %s", msg);

                // Parse only what we need:
                // {"type":"event","payload":{"command":"ota_update, fw=1"}}
                cJSON *root = cJSON_Parse(msg);
                if (root) {
                    cJSON *jt = cJSON_GetObjectItem(root, "type");
                    if (cJSON_IsString(jt) && jt->valuestring && strcmp(jt->valuestring, "event") == 0) {
                        cJSON *pl = cJSON_GetObjectItem(root, "payload");
                        cJSON *cmd = pl ? cJSON_GetObjectItem(pl, "command") : NULL;
                        if (cJSON_IsString(cmd) && cmd->valuestring && s_cb) {
                            // callback is expected to be fast; it should spawn tasks if doing work
                            s_cb(jt->valuestring, cmd->valuestring);
                        }
                    }
                    cJSON_Delete(root);
                }

                free(msg);
            } else if (d->op_code == 0x2 /* binary */) {
                ESP_LOGI(TAG, "RX: binary frame");
                log_hex_first64((const uint8_t *)d->data_ptr, d->data_len);
            } else {
                ESP_LOGI(TAG, "RX: op=%d len=%d", (int)d->op_code, (int)d->data_len);
            }
            break;
        }

        default:
            break;
    }
}

esp_err_t ws_client_start(const char *api_base, const char *bearer_token, ws_event_cb_t cb)
{
    if (!api_base || !bearer_token) return ESP_ERR_INVALID_ARG;
    if (s_ws) {
        ESP_LOGW(TAG, "WS already started");
        return ESP_OK;
    }

    s_cb = cb;
    strncpy(s_api_base, api_base, sizeof(s_api_base) - 1);

    char ws_url[256];
    build_ws_url_from_api_base(api_base, ws_url, sizeof(ws_url));

    // headers must persist while the client exists
    char *headers = (char *)calloc(1, 900);
    if (!headers) return ESP_ERR_NO_MEM;

    snprintf(headers, 900,
             "Authorization: Bearer %s\r\n"
             "Origin: %s\r\n"
             "\r\n",
             bearer_token, api_base);

    esp_websocket_client_config_t cfg = {
        .uri = ws_url,
        .headers = headers, // keep alive (do not free)
        .crt_bundle_attach = esp_crt_bundle_attach,
        .network_timeout_ms = 30000,
        .reconnect_timeout_ms = 5000,
        .buffer_size = 2048,
    };

    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        free(headers);
        return ESP_FAIL;
    }

    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    ESP_LOGI(TAG, "Starting WS: %s", ws_url);
    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        free(headers);
        return err;
    }

    // keepalive task (can be suspended during OTA)
    xTaskCreate(ws_keepalive_task_fn, "ws_keepalive", 4096, NULL, 4, &s_keepalive_task);

    return ESP_OK;
}

void ws_client_stop(void)
{
    if (!s_ws) return;

    if (s_keepalive_task) {
        vTaskDelete(s_keepalive_task);
        s_keepalive_task = NULL;
    }

    esp_websocket_client_stop(s_ws);
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;

    // Note: headers memory was allocated in ws_client_start and is intentionally not freed here
    // unless you store it in a static pointer. If you want zero leak, store headers in static and free here.

    if (s_ws_tx_mu) {
        vSemaphoreDelete(s_ws_tx_mu);
        s_ws_tx_mu = NULL;
    }

    ESP_LOGI(TAG, "WS stopped");
}