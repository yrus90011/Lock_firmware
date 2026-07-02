#include "access_log_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "api_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define ACCESS_LOG_CAPACITY 200
#define ACCESS_LOG_QUEUE_DEPTH 16
#define ACCESS_LOG_NS "alogq"

typedef struct {
    char uid[40];
    char result[8];
    char reason[48];
    char ts[32];
    bool omit_ts;
} access_log_record_t;

typedef struct {
    char api_base[192];
    char device_uuid[64];
    char device_secret[128];
    char token[768];
} access_log_ctx_t;

static const char *TAG = "ACCESS_LOG";

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_ctx_mu;
static TaskHandle_t s_task;
static access_log_ctx_t s_ctx;
static access_log_ready_cb_t s_ready_cb;
static access_log_start_backend_cb_t s_start_backend_cb;

static void copy_trunc_c(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) src = "";

    size_t i = 0;
    while (i + 1 < dst_len && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static bool get_iso8601_utc_now(char *dst, size_t dst_len)
{
    time_t now = time(NULL);
    if (now < 1600000000) return false;

    struct tm tm;
    if (!gmtime_r(&now, &tm)) return false;
    return strftime(dst, dst_len, "%Y-%m-%dT%H:%M:%SZ", &tm) > 0;
}

static void key_for_slot(char key[8], uint16_t slot)
{
    snprintf(key, 8, "r%03u", (unsigned)slot);
}

static esp_err_t nvs_get_u16_default(nvs_handle_t h, const char *key, uint16_t *out, uint16_t def)
{
    uint16_t v = def;
    esp_err_t err = nvs_get_u16(h, key, &v);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK && out) *out = v;
    return err;
}

static esp_err_t load_meta(nvs_handle_t h, uint16_t *head, uint16_t *count)
{
    esp_err_t err = nvs_get_u16_default(h, "head", head, 0);
    if (err != ESP_OK) return err;

    err = nvs_get_u16_default(h, "count", count, 0);
    if (err != ESP_OK) return err;

    if (*head >= ACCESS_LOG_CAPACITY) *head = 0;
    if (*count > ACCESS_LOG_CAPACITY) *count = ACCESS_LOG_CAPACITY;
    return ESP_OK;
}

static esp_err_t save_meta(nvs_handle_t h, uint16_t head, uint16_t count)
{
    esp_err_t err = nvs_set_u16(h, "head", head);
    if (err != ESP_OK) return err;
    return nvs_set_u16(h, "count", count);
}

static esp_err_t persist_record(const access_log_record_t *rec)
{
    if (!rec) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(ACCESS_LOG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    uint16_t head = 0;
    uint16_t count = 0;
    err = load_meta(h, &head, &count);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    if (count >= ACCESS_LOG_CAPACITY) {
        char old_key[8];
        key_for_slot(old_key, head);
        (void)nvs_erase_key(h, old_key);
        head = (head + 1) % ACCESS_LOG_CAPACITY;
        count--;
    }

    uint16_t slot = (head + count) % ACCESS_LOG_CAPACITY;
    char key[8];
    key_for_slot(key, slot);

    err = nvs_set_blob(h, key, rec, sizeof(*rec));
    if (err == ESP_OK) {
        count++;
        err = save_meta(h, head, count);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Queued access log uid=%s result=%s pending=%u",
                 rec->uid, rec->result, (unsigned)count);
    }
    return err;
}

static esp_err_t peek_record(access_log_record_t *rec, uint16_t *out_count)
{
    if (!rec) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(ACCESS_LOG_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    uint16_t head = 0;
    uint16_t count = 0;
    err = load_meta(h, &head, &count);
    if (err != ESP_OK || count == 0) {
        nvs_close(h);
        if (out_count) *out_count = count;
        return count == 0 ? ESP_ERR_NOT_FOUND : err;
    }

    char key[8];
    key_for_slot(key, head);
    size_t len = sizeof(*rec);
    err = nvs_get_blob(h, key, rec, &len);
    nvs_close(h);

    if (out_count) *out_count = count;
    if (err == ESP_OK && len != sizeof(*rec)) return ESP_ERR_INVALID_SIZE;
    return err;
}

static esp_err_t pop_record(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ACCESS_LOG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    uint16_t head = 0;
    uint16_t count = 0;
    err = load_meta(h, &head, &count);
    if (err != ESP_OK || count == 0) {
        nvs_close(h);
        return count == 0 ? ESP_ERR_NOT_FOUND : err;
    }

    char key[8];
    key_for_slot(key, head);
    (void)nvs_erase_key(h, key);

    head = (head + 1) % ACCESS_LOG_CAPACITY;
    count--;
    err = save_meta(h, head, count);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool copy_context(access_log_ctx_t *out)
{
    if (!out || !s_ctx_mu) return false;

    xSemaphoreTake(s_ctx_mu, portMAX_DELAY);
    *out = s_ctx;
    xSemaphoreGive(s_ctx_mu);

    return out->api_base[0] && out->device_uuid[0] &&
           out->device_secret[0] && out->token[0];
}

static bool service_ready(void)
{
    return s_ready_cb && s_ready_cb();
}

static void drain_ram_queue(void)
{
    access_log_record_t rec;
    while (s_queue && xQueueReceive(s_queue, &rec, 0) == pdTRUE) {
        esp_err_t err = persist_record(&rec);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Persist access log failed: %s", esp_err_to_name(err));
        }
    }
}

static bool upload_one(void)
{
    access_log_record_t rec;
    uint16_t pending = 0;
    esp_err_t err = peek_record(&rec, &pending);
    if (err == ESP_ERR_NOT_FOUND) return false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Peek access log failed: %s", esp_err_to_name(err));
        return false;
    }

    access_log_ctx_t ctx;
    if (!copy_context(&ctx) || !service_ready()) return false;

    bool known_member = true;
    err = api_post_lock_access_log(
        ctx.api_base,
        ctx.device_uuid,
        ctx.token,
        sizeof(ctx.token),
        ctx.device_secret,
        rec.uid,
        rec.result,
        "IN",
        "NFC",
        rec.reason,
        rec.uid,
        rec.omit_ts ? NULL : rec.ts,
        rec.omit_ts,
        &known_member);

    access_log_service_set_context(ctx.api_base, ctx.device_uuid, ctx.device_secret, ctx.token);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Upload access log failed uid=%s result=%s pending=%u err=%s",
                 rec.uid, rec.result, (unsigned)pending, esp_err_to_name(err));
        return false;
    }

    err = pop_record();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Pop access log failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!known_member) {
        ESP_LOGW(TAG, "Backend reports unknown member for uid=%s", rec.uid);
    }

    ESP_LOGI(TAG, "Uploaded access log uid=%s result=%s pending=%u",
             rec.uid, rec.result, pending > 0 ? (unsigned)(pending - 1) : 0);
    return true;
}

static void access_log_task(void *arg)
{
    (void)arg;

    while (1) {
        drain_ram_queue();

        if (service_ready()) {
            while (service_ready() && upload_one()) {
                drain_ram_queue();
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        } else if (s_start_backend_cb) {
            s_start_backend_cb();
        }

        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    }
}

esp_err_t access_log_service_start(access_log_ready_cb_t ready_cb,
                                   access_log_start_backend_cb_t start_backend_cb)
{
    s_ready_cb = ready_cb;
    s_start_backend_cb = start_backend_cb;

    if (!s_ctx_mu) {
        s_ctx_mu = xSemaphoreCreateMutex();
        if (!s_ctx_mu) return ESP_ERR_NO_MEM;
    }

    if (!s_queue) {
        s_queue = xQueueCreate(ACCESS_LOG_QUEUE_DEPTH, sizeof(access_log_record_t));
        if (!s_queue) return ESP_ERR_NO_MEM;
    }

    if (!s_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(
            access_log_task,
            "access_log_svc",
            8192,
            NULL,
            2,
            &s_task,
            0);
        if (ok != pdPASS) {
            s_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

void access_log_service_set_context(const char *api_base,
                                    const char *device_uuid,
                                    const char *device_secret,
                                    const char *token)
{
    if (!s_ctx_mu) return;

    xSemaphoreTake(s_ctx_mu, portMAX_DELAY);
    copy_trunc_c(s_ctx.api_base, sizeof(s_ctx.api_base), api_base);
    copy_trunc_c(s_ctx.device_uuid, sizeof(s_ctx.device_uuid), device_uuid);
    copy_trunc_c(s_ctx.device_secret, sizeof(s_ctx.device_secret), device_secret);
    copy_trunc_c(s_ctx.token, sizeof(s_ctx.token), token);
    xSemaphoreGive(s_ctx_mu);
}

esp_err_t access_log_service_enqueue(const char *uid,
                                     const char *result,
                                     const char *reason,
                                     bool omit_ts)
{
    if (!uid || !result || !reason) return ESP_ERR_INVALID_ARG;

    access_log_record_t rec = {0};
    copy_trunc_c(rec.uid, sizeof(rec.uid), uid);
    copy_trunc_c(rec.result, sizeof(rec.result), result);
    copy_trunc_c(rec.reason, sizeof(rec.reason), reason);
    rec.omit_ts = omit_ts;

    if (!omit_ts && !get_iso8601_utc_now(rec.ts, sizeof(rec.ts))) {
        rec.omit_ts = true;
    }

    if (!s_queue) return ESP_ERR_INVALID_STATE;

    if (xQueueSend(s_queue, &rec, 0) != pdTRUE) {
        ESP_LOGW(TAG, "RAM queue full, dropping access log uid=%s result=%s", rec.uid, rec.result);
        return ESP_ERR_TIMEOUT;
    }

    if (s_task) xTaskNotifyGive(s_task);
    return ESP_OK;
}
