#include "ws_log_upload.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"

#include "cJSON.h"
#include "ws_client.h"   // uses ws_client_send_text()

static const char *TAG = "LOG_UPLOAD";

// IMPORTANT: your ws_client config has buffer_size=2048.
// Keep each JSON message < ~1800 bytes to be very safe.
#define RAW_CHUNK_BYTES          1024
#define READY_WAIT_MS           10000
#define DONE_WAIT_MS            15000
#define AFTER_CHUNK_DELAY_MS      10

#define UPLOAD_ID_MAX              64
#define ERR_MAX                    96

typedef struct {
  SemaphoreHandle_t mu;
  SemaphoreHandle_t sem_ready;
  SemaphoreHandle_t sem_done;
  SemaphoreHandle_t sem_error;

  char upload_id[UPLOAD_ID_MAX];
  char last_error[ERR_MAX];
  bool active;
} upload_ctx_t;

static upload_ctx_t g = {0};

static void ctx_init_once(void) {
  if (g.mu) return;
  g.mu = xSemaphoreCreateMutex();
  g.sem_ready = xSemaphoreCreateBinary();
  g.sem_done  = xSemaphoreCreateBinary();
  g.sem_error = xSemaphoreCreateBinary();
}

static void ctx_reset(void) {
  xSemaphoreTake(g.mu, portMAX_DELAY);
  g.upload_id[0] = 0;
  g.last_error[0] = 0;
  g.active = true;
  xSemaphoreGive(g.mu);

  (void)xSemaphoreTake(g.sem_ready, 0);
  (void)xSemaphoreTake(g.sem_done, 0);
  (void)xSemaphoreTake(g.sem_error, 0);
}

static void ctx_finish(void) {
  xSemaphoreTake(g.mu, portMAX_DELAY);
  g.active = false;
  xSemaphoreGive(g.mu);
}

static esp_err_t sha256_file_hex(const char *path, char out_hex[65]) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return ESP_FAIL;

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);   // 0 = SHA-256, 1 = SHA-224

  unsigned char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
    mbedtls_sha256_update(&ctx, buf, n);
  }

  unsigned char hash[32];
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  fclose(fp);

  static const char *hex = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out_hex[i * 2 + 0] = hex[(hash[i] >> 4) & 0xF];
    out_hex[i * 2 + 1] = hex[(hash[i] >> 0) & 0xF];
  }
  out_hex[64] = '\0';
  return ESP_OK;
}

// ===== called from ws_client.c on each complete JSON message =====
void ws_log_upload_on_rx_json(const char *json_str) {
  if (!json_str) return;
  if (!g.mu) return;

  cJSON *root = cJSON_Parse(json_str);
  if (!root) return;

  cJSON *jt = cJSON_GetObjectItem(root, "type");
  cJSON *pl = cJSON_GetObjectItem(root, "payload");
  if (!cJSON_IsString(jt) || !cJSON_IsObject(pl)) {
    cJSON_Delete(root);
    return;
  }

  xSemaphoreTake(g.mu, portMAX_DELAY);
  bool active = g.active;
  xSemaphoreGive(g.mu);
  if (!active) {
    cJSON_Delete(root);
    return;
  }

  if (strcmp(jt->valuestring, "log_upload_ready") == 0) {
    cJSON *uid = cJSON_GetObjectItem(pl, "upload_id");
    if (cJSON_IsString(uid)) {
      xSemaphoreTake(g.mu, portMAX_DELAY);
      strlcpy(g.upload_id, uid->valuestring, sizeof(g.upload_id));
      xSemaphoreGive(g.mu);
      xSemaphoreGive(g.sem_ready);
    }
  } else if (strcmp(jt->valuestring, "log_upload_done") == 0) {
    xSemaphoreGive(g.sem_done);
  } else if (strcmp(jt->valuestring, "log_upload_error") == 0) {
    cJSON *err = cJSON_GetObjectItem(pl, "error");
    xSemaphoreTake(g.mu, portMAX_DELAY);
    strlcpy(g.last_error, cJSON_IsString(err) ? err->valuestring : "unknown_error",
            sizeof(g.last_error));
    xSemaphoreGive(g.mu);
    xSemaphoreGive(g.sem_error);
  }

  cJSON_Delete(root);
}

esp_err_t ws_upload_log_file(const char *fs_path, const char *remote_filename) {
  ctx_init_once();

  if (!fs_path || !remote_filename) return ESP_ERR_INVALID_ARG;

  struct stat st;
  if (stat(fs_path, &st) != 0) {
    ESP_LOGE(TAG, "stat failed: %s", fs_path);
    return ESP_ERR_NOT_FOUND;
  }
  size_t file_size = (size_t)st.st_size;
  if (file_size == 0) {
    ESP_LOGW(TAG, "empty file: %s", fs_path);
    return ESP_OK;
  }

  // Optional SHA
  bool do_sha = true;
  char sha_hex[65] = {0};
  if (do_sha) {
    if (sha256_file_hex(fs_path, sha_hex) != ESP_OK) sha_hex[0] = 0;
  }

  ctx_reset();

  // BEGIN
  char begin_json[512];
  if (sha_hex[0]) {
    snprintf(begin_json, sizeof(begin_json),
      "{\"type\":\"log_upload_begin\",\"payload\":{\"filename\":\"%s\",\"content_type\":\"text/plain\",\"size\":%u,\"sha256\":\"%s\"}}",
      remote_filename, (unsigned)file_size, sha_hex);
  } else {
    snprintf(begin_json, sizeof(begin_json),
      "{\"type\":\"log_upload_begin\",\"payload\":{\"filename\":\"%s\",\"content_type\":\"text/plain\",\"size\":%u}}",
      remote_filename, (unsigned)file_size);
  }

  ESP_ERROR_CHECK_WITHOUT_ABORT(ws_client_send_text(begin_json));

  // wait READY
  if (xSemaphoreTake(g.sem_ready, pdMS_TO_TICKS(READY_WAIT_MS)) != pdTRUE) {
    ESP_LOGE(TAG, "timeout waiting ready");
    ctx_finish();
    return ESP_ERR_TIMEOUT;
  }

  char upload_id[UPLOAD_ID_MAX];
  xSemaphoreTake(g.mu, portMAX_DELAY);
  strlcpy(upload_id, g.upload_id, sizeof(upload_id));
  xSemaphoreGive(g.mu);

  if (!upload_id[0]) {
    ESP_LOGE(TAG, "no upload_id");
    ctx_finish();
    return ESP_FAIL;
  }

  FILE *fp = fopen(fs_path, "rb");
  if (!fp) {
    ESP_LOGE(TAG, "open failed: %s", fs_path);
    ctx_finish();
    return ESP_FAIL;
  }

  uint8_t raw[RAW_CHUNK_BYTES];
  char b64[4 * ((RAW_CHUNK_BYTES + 2) / 3) + 1];
  // keep JSON small
  char chunk_json[1900];

  size_t sent_total = 0;

  while (1) {
    size_t n = fread(raw, 1, sizeof(raw), fp);
    if (n == 0) break;

    size_t b64_len = 0;
    int rc = mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &b64_len, raw, n);
    if (rc != 0) {
      ESP_LOGE(TAG, "base64 rc=%d", rc);
      fclose(fp);
      ctx_finish();
      return ESP_FAIL;
    }
    b64[b64_len] = 0;

    int m = snprintf(chunk_json, sizeof(chunk_json),
      "{\"type\":\"log_upload_chunk\",\"payload\":{\"upload_id\":\"%s\",\"data_b64\":\"%s\"}}",
      upload_id, b64);

    if (m <= 0 || m >= (int)sizeof(chunk_json)) {
      ESP_LOGE(TAG, "chunk json overflow (reduce RAW_CHUNK_BYTES)");
      fclose(fp);
      ctx_finish();
      return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ws_client_send_text(chunk_json);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "send chunk failed: %s", esp_err_to_name(err));
      fclose(fp);
      ctx_finish();
      return err;
    }

    sent_total += n;

    // abort on server error
    if (xSemaphoreTake(g.sem_error, 0) == pdTRUE) {
      xSemaphoreTake(g.mu, portMAX_DELAY);
      ESP_LOGE(TAG, "server error: %s", g.last_error);
      xSemaphoreGive(g.mu);
      fclose(fp);
      ctx_finish();
      return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS(AFTER_CHUNK_DELAY_MS));
  }

  fclose(fp);

  // END
  char end_json[192];
  snprintf(end_json, sizeof(end_json),
    "{\"type\":\"log_upload_end\",\"payload\":{\"upload_id\":\"%s\"}}",
    upload_id);

  ESP_ERROR_CHECK_WITHOUT_ABORT(ws_client_send_text(end_json));

  // wait DONE or ERROR
  TickType_t t0 = xTaskGetTickCount();
  while ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(DONE_WAIT_MS)) {
    if (xSemaphoreTake(g.sem_done, 0) == pdTRUE) {
      ESP_LOGI(TAG, "upload done: %s (%u bytes)", upload_id, (unsigned)sent_total);
      ctx_finish();
      return ESP_OK;
    }
    if (xSemaphoreTake(g.sem_error, 0) == pdTRUE) {
      xSemaphoreTake(g.mu, portMAX_DELAY);
      ESP_LOGE(TAG, "upload failed: %s", g.last_error);
      xSemaphoreGive(g.mu);
      ctx_finish();
      return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  ESP_LOGE(TAG, "timeout waiting done");
  ctx_finish();
  return ESP_ERR_TIMEOUT;
}