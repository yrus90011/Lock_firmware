#include "evtlog.h"

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

static const char *TAG = "EVTLOG";

#define EVTLOG_NVS_NS        "evtlog"
#define EVTLOG_NVS_KEY_STATE "state"
#define EVTLOG_WINDOW_SEC    (6 * 60 * 60)

#define EVTLOG_QUEUE_LEN     16
#define EVTLOG_COMMIT_MIN_MS 3000   // batch commits (flash wear)
#define EVTLOG_TICK_MS       1000   // periodic window check

static QueueHandle_t s_q;
static TaskHandle_t  s_task;
static evtlog_state_t s_state;
static nvs_handle_t s_nvs = 0;

static uint32_t now_epoch_or_zero(void) {
    time_t now = 0;
    time(&now);
    // If SNTP not set yet, epoch often near 0 or 1970; treat as 0.
    if (now < 1700000000) return 0; // ~2023-11; adjust if you want
    return (uint32_t)now;
}

// Fallback “seconds since boot” so we can still do 6h windows even without real time
static uint32_t monotonic_sec(void) {
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static void reset_window(uint32_t start_epoch) {
    s_state.window_start_epoch = start_epoch;
    s_state.boot_count = 0;
    s_state.ws_disc_count = 0;
    s_state.nfc_fail_count = 0;
    s_state.mpr_fail_count = 0;
}

static esp_err_t load_state_from_nvs(void) {
    size_t sz = sizeof(s_state);
    esp_err_t err = nvs_get_blob(s_nvs, EVTLOG_NVS_KEY_STATE, &s_state, &sz);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(&s_state, 0, sizeof(s_state));
        return ESP_OK;
    }
    return err;
}

static esp_err_t save_state_to_nvs(void) {
    esp_err_t err = nvs_set_blob(s_nvs, EVTLOG_NVS_KEY_STATE, &s_state, sizeof(s_state));
    if (err != ESP_OK) return err;
    return nvs_commit(s_nvs);
}

static void apply_event(evtlog_type_t t) {
    switch (t) {
        case EVT_BOOT:         s_state.boot_count++;      break;
        case EVT_WS_DISCONNECT:s_state.ws_disc_count++;   break;
        case EVT_NFC_FAIL:     s_state.nfc_fail_count++;  break;
        case EVT_MPR_FAIL:     s_state.mpr_fail_count++;  break;
        default: break;
    }
}

static void evtlog_task(void *arg) {
    (void)arg;

    TickType_t last_commit = xTaskGetTickCount();
    bool dirty = false;

    while (true) {
        // 1) Wait for an event up to EVTLOG_TICK_MS
        evtlog_type_t ev;
        if (xQueueReceive(s_q, &ev, pdMS_TO_TICKS(EVTLOG_TICK_MS)) == pdTRUE) {
            // 2) Window check before applying
            uint32_t epoch = now_epoch_or_zero();
            uint32_t ref_now = epoch ? epoch : monotonic_sec();

            uint32_t ref_start = s_state.window_start_epoch;
            if (ref_start == 0) {
                // initialize if empty
                s_state.window_start_epoch = ref_now;
                ref_start = ref_now;
                dirty = true;
            }

            if ((ref_now - ref_start) >= EVTLOG_WINDOW_SEC) {
                // start a fresh 6h window
                reset_window(ref_now);
                dirty = true;
            }

            // 3) Apply event
            apply_event(ev);
            s_state.last_update_epoch = epoch; // 0 if not synced
            dirty = true;
        } else {
            // Timeout tick: only do window rollover check
            uint32_t epoch = now_epoch_or_zero();
            uint32_t ref_now = epoch ? epoch : monotonic_sec();

            uint32_t ref_start = s_state.window_start_epoch;
            if (ref_start != 0 && (ref_now - ref_start) >= EVTLOG_WINDOW_SEC) {
                reset_window(ref_now);
                s_state.last_update_epoch = epoch;
                dirty = true;
            }
        }

        // 4) Batch commits
        if (dirty) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_commit) >= pdMS_TO_TICKS(EVTLOG_COMMIT_MIN_MS)) {
                esp_err_t err = save_state_to_nvs();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "NVS save failed: %s", esp_err_to_name(err));
                }
                last_commit = now;
                dirty = false;
            }
        }
    }
}

esp_err_t evtlog_init_and_start(void) {
    // Create queue early
    if (!s_q) s_q = xQueueCreate(EVTLOG_QUEUE_LEN, sizeof(evtlog_type_t));
    if (!s_q) return ESP_ERR_NO_MEM;

    esp_err_t err = nvs_open(EVTLOG_NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) return err;

    err = load_state_from_nvs();
    if (err != ESP_OK) return err;

    // FIRST THING: store boot time + reset reason + window reset if needed
    uint32_t epoch = now_epoch_or_zero();
    uint32_t ref_now = epoch ? epoch : monotonic_sec();

    s_state.boot_time_epoch = epoch;                 // 0 if not synced yet
    s_state.last_reset_reason = (uint32_t)esp_reset_reason();

    // enforce 6h window at boot
    if (s_state.window_start_epoch == 0 ||
        (ref_now - s_state.window_start_epoch) >= EVTLOG_WINDOW_SEC) {
        reset_window(ref_now);
    }

    // count this boot as an event
    apply_event(EVT_BOOT);
    s_state.last_update_epoch = epoch;

    // save immediately once (so you never miss the boot record)
    err = save_state_to_nvs();
    if (err != ESP_OK) return err;

    // Start task (once)
    if (!s_task) {
        xTaskCreatePinnedToCore(evtlog_task, "evtlog", 4096, NULL, 5, &s_task, 1);
    }

    return ESP_OK;
}

void evtlog_push(evtlog_type_t t) {
    if (!s_q) return;
    (void)xQueueSend(s_q, &t, 0);
}

esp_err_t evtlog_get_state(evtlog_state_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memcpy(out, &s_state, sizeof(*out));
    return ESP_OK;
}