#include "ota_client.h"
#include "api_client.h"
#include "nvs_config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "mbedtls/sha256.h"

#include <stdbool.h>
#include "task_control.h"
#include "ws_client.h"

static volatile bool s_ota_running = false;
// You can call these from main.c; declare them extern if needed
extern void app_tasks_resume_all(void);
extern void ws_client_resume_aux_tasks(void);
extern esp_err_t ws_client_send_text(const char *text);

static const char *TAG = "OTA";


bool ota_client_is_running(void)
{
    return s_ota_running;
}

static void sha256_to_hex(const uint8_t hash[32], char out_hex[65]) {
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2 + 0] = hex[(hash[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = hex[(hash[i] >> 0) & 0xF];
    }
    out_hex[64] = '\0';
}

void ota_mark_valid_if_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "PENDING_VERIFY: marking app valid to cancel rollback.");
            ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        }
    }
}

static esp_err_t ota_stream_download_and_flash(const char *api_base,
                                               int fw_id,
                                               const char *bearer_token,
                                               const char *expected_sha256_hex,
                                               int expected_file_size)
{
    app_tasks_suspend_all();
    char url[320];
    snprintf(url, sizeof(url), "%s/api/v1/firmware/%d/download", api_base, fw_id);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "accept", "application/octet-stream");
    esp_http_client_set_header(client, "Connection", "close");

    char auth[600];
    snprintf(auth, sizeof(auth), "Bearer %s", bearer_token);
    esp_http_client_set_header(client, "Authorization", auth);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    int64_t cl = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "download status=%d content_length=%" PRId64, status, cl);

    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    if (expected_file_size <= 0 && cl > 0 && cl < INT32_MAX) {
        expected_file_size = (int)cl;
    }

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        ESP_LOGE(TAG, "No OTA update partition found");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Writing to %s @ 0x%lx", update->label, (unsigned long)update->address);

    esp_ota_handle_t ota = 0;
    err = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) {
        mbedtls_sha256_free(&sha);
        esp_ota_abort(ota);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    bool header_ok = false;
    int total = 0;
    bool ok = false;
    int last_pct = -1;

    while (1) {
        int r = esp_http_client_read(client, (char *)buf, 4096);

        if (r < 0) { ESP_LOGE(TAG, "read error r=%d", r); break; }
        if (r == 0) {
            if (esp_http_client_is_complete_data_received(client)) { ok = true; break; }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (!header_ok) {
            int need = (int)(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t));
            if (r < need) { ESP_LOGE(TAG, "first chunk too small (%d < %d)", r, need); break; }
            esp_image_header_t *hdr = (esp_image_header_t *)buf;
            if (hdr->magic != ESP_IMAGE_HEADER_MAGIC) { ESP_LOGE(TAG, "bad image magic 0x%02x", hdr->magic); break; }
            header_ok = true;
        }

        mbedtls_sha256_update(&sha, buf, r);

        err = esp_ota_write(ota, buf, r);
        if (err != ESP_OK) { ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err)); break; }

        total += r;

        if (expected_file_size > 0) {
            int pct = (int)(((int64_t)total * 100) / expected_file_size);
            if (pct > 100) pct = 100;
            if (pct >= last_pct + 5) { 
                ESP_LOGI(TAG, "downloaded %d%% (%d/%d)", pct, total, expected_file_size);               
                char ws_msg[256];
                snprintf(ws_msg, sizeof(ws_msg),
                        "{\"type\":\"event\",\"payload\":{"
                        "\"response\":\"ota_progress\","
                        "\"pct\":%d,"
                        "\"received\":%d,"
                        "\"total\":%d,"
                        "\"text\":\"downloaded %d%% (%d/%d)\""
                        "}}",
                        pct, total, expected_file_size,
                        pct, total, expected_file_size);

                (void)ws_send_text_locked(ws_msg);
                last_pct = pct;
            }
        } else if ((total & ((64 * 1024) - 1)) == 0) {
            ESP_LOGI(TAG, "downloaded %d bytes", total);
        }
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!ok) {
        ESP_LOGE(TAG, "stream failed -> abort");
        mbedtls_sha256_free(&sha);
        esp_ota_abort(ota);
        return ESP_FAIL;
    }

    if (expected_file_size > 0 && total != expected_file_size) {
        ESP_LOGE(TAG, "incomplete: got=%d expected=%d", total, expected_file_size);
        mbedtls_sha256_free(&sha);
        esp_ota_abort(ota);
        return ESP_FAIL;
    }

    uint8_t hash[32];
    mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);

    char got_hex[65];
    sha256_to_hex(hash, got_hex);
    ESP_LOGI(TAG, "sha256 got=%s", got_hex);
    ESP_LOGI(TAG, "sha256 exp=%s", expected_sha256_hex);

    if (strcasecmp(got_hex, expected_sha256_hex) != 0) {
        ESP_LOGE(TAG, "SHA256 mismatch -> abort");
        esp_ota_abort(ota);
        return ESP_FAIL;
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "OTA success. Rebooting...");
    (void)ws_client_send_text("{\"type\":\"event\",\"payload\":{\"response\":\"Rebooting...\"}}");

    vTaskDelay(pdMS_TO_TICKS(800));
    app_tasks_resume_all();
    esp_restart();
    return ESP_OK;
}

typedef struct {
    char api_base[192];
    char device_uuid[64];
    char token[768];
} ota_task_args_t;

static void ota_task(void *arg)
{
    ota_task_args_t *a = (ota_task_args_t *)arg;

    firmware_info_t fw;
    esp_err_t err = api_get_latest_firmware(a->api_base, a->device_uuid, a->token, &fw);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "No firmware available for device.");
        goto done;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_latest_firmware failed: %s", esp_err_to_name(err));
        goto done;
    }

    int last_fw = -1;
    if (nvs_config_get_last_fw_id(&last_fw) == ESP_OK && last_fw == fw.id) {
        ESP_LOGI(TAG, "Latest fw id=%d already applied (last_fw_id). Skipping.", fw.id);
        goto done;
    }

    // Try up to 3 full retries
    for (int i = 1; i <= 3; i++) {
        ESP_LOGW(TAG, "OTA attempt %d/3 fw_id=%d", i, fw.id);
        err = ota_stream_download_and_flash(a->api_base, fw.id, a->token, fw.sha256_hex, fw.file_size);
        if (err == ESP_OK) {
            // reboot happens inside on success; this line usually not reached
            (void)nvs_config_set_last_fw_id(fw.id);
            break;
        }
        ESP_LOGW(TAG, "OTA attempt %d failed: %s", i, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

done:
    free(a);
    vTaskDelete(NULL);
}

esp_err_t ota_client_start(const char *api_base,
                           const char *device_uuid,
                           const char *bearer_token)
{
    if (!api_base || !device_uuid || !bearer_token) return ESP_ERR_INVALID_ARG;

    ota_task_args_t *a = (ota_task_args_t *)calloc(1, sizeof(*a));
    if (!a) return ESP_ERR_NO_MEM;

    strncpy(a->api_base, api_base, sizeof(a->api_base) - 1);
    strncpy(a->device_uuid, device_uuid, sizeof(a->device_uuid) - 1);
    strncpy(a->token, bearer_token, sizeof(a->token) - 1);

    // OTA task stack: 12–16KB recommended (you requested). This task avoids huge stack buffers.
    BaseType_t ok = xTaskCreate(ota_task, "ota_task", 16 * 1024, a, 5, NULL);
    if (ok != pdPASS) {
        free(a);
        return ESP_FAIL;
    }
    return ESP_OK;
}

typedef struct {
    char api_base[192];
    char device_uuid[64];
    char token[768];
    int  fw_id;
} ota_by_id_args_t;

static void ota_by_id_task(void *arg)
{
    ota_by_id_args_t *a = (ota_by_id_args_t *)arg;

    firmware_info_t fw;
    esp_err_t err = api_get_firmware_by_id(a->api_base, a->device_uuid, a->token, a->fw_id, &fw);
    if (err == ESP_ERR_NOT_FOUND) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"event\",\"payload\":{\"response\":\"ota_result\",\"status\":\"error\",\"fw\":%d,\"message\":\"fw id does not exist\"}}",
                 a->fw_id);
        (void)ws_client_send_text(msg);
        s_ota_running = false;
        free(a);
        vTaskDelete(NULL);
    }

    if (err != ESP_OK) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"event\",\"payload\":{\"response\":\"ota_result\",\"status\":\"error\",\"fw\":%d,\"message\":\"fw lookup failed\"}}",
                 a->fw_id);
        (void)ws_client_send_text(msg);
        s_ota_running = false;
        free(a);
        vTaskDelete(NULL);
    }

    // Found fw -> start downloading + flashing (this reboots on success)
    char start_msg[256];
    snprintf(start_msg, sizeof(start_msg),
             "{\"type\":\"event\",\"payload\":{\"response\":\"ota_result\",\"status\":\"running\",\"fw\":%d,\"message\":\"downloading\"}}",
             a->fw_id);
    (void)ws_client_send_text(start_msg);

    // call your internal OTA streamer
    err = ota_stream_download_and_flash(a->api_base, fw.id, a->token, fw.sha256_hex, fw.file_size);

    // If OTA succeeds, device reboots, so code below usually runs only on failure
    char end_msg[256];
    snprintf(end_msg, sizeof(end_msg),
             "{\"type\":\"event\",\"payload\":{\"response\":\"ota_result\",\"status\":\"error\",\"fw\":%d,\"message\":\"ota failed\"}}",
             a->fw_id);
    (void)ws_client_send_text(end_msg);
    s_ota_running = false;

    free(a);
    vTaskDelete(NULL);
}

esp_err_t ota_client_start_by_id(const char *api_base,
                                 const char *device_uuid,
                                 const char *bearer_token,
                                 int fw_id)
{
    if (!api_base || !device_uuid || !bearer_token || fw_id <= 0) return ESP_ERR_INVALID_ARG;

    ota_by_id_args_t *a = (ota_by_id_args_t *)calloc(1, sizeof(*a));
    if (!a) return ESP_ERR_NO_MEM;

    strncpy(a->api_base, api_base, sizeof(a->api_base) - 1);
    strncpy(a->device_uuid, device_uuid, sizeof(a->device_uuid) - 1);
    strncpy(a->token, bearer_token, sizeof(a->token) - 1);
    a->fw_id = fw_id;

    if (s_ota_running) return ESP_ERR_INVALID_STATE;
    s_ota_running = true;

    if (xTaskCreate(ota_by_id_task, "ota_by_id", 16 * 1024, a, 5, NULL) != pdPASS) {
        free(a);
        return ESP_FAIL;
    }
    return ESP_OK;
}