#include "monitor.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include "esp_rom_sys.h"   // esp_rom_printf / esp_rom_vprintf
#include <stdarg.h>

static TaskHandle_t s_monitor_task = NULL;

static const char *TAG = "MONITOR";

static void monitor_uart_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    esp_rom_vprintf(fmt, ap);
    va_end(ap);
    esp_rom_printf("\n");
}
static void snapshot_log(const monitor_snapshot_t *s)
{
    monitor_uart_printf("I (%u) MONITOR: HEAP 8BIT: free=%u min=%u lfb=%u",
        (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS),
        (unsigned)s->free8, (unsigned)s->min8, (unsigned)s->lfb8);

    monitor_uart_printf("I (%u) MONITOR: HEAP INT : free=%u min=%u lfb=%u",
        (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS),
        (unsigned)s->free_int, (unsigned)s->min_int, (unsigned)s->lfb_int);

    monitor_uart_printf("I (%u) MONITOR: TASK STACKS (high water bytes) count=%u",
        (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS),
        (unsigned)s->task_count);

    for (uint32_t i = 0; i < s->task_count; i++) {
        monitor_uart_printf("I (%u) MONITOR:   %-16s prio=%u hwm=%u state=%u",
            (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS),
            s->tasks[i].name,
            (unsigned)s->tasks[i].prio,
            (unsigned)s->tasks[i].hwm_bytes,
            (unsigned)s->tasks[i].state);

        // yield every few lines so we don't hog CPU
        if ((i & 3u) == 3u) vTaskDelay(pdMS_TO_TICKS(1));
    }
}

int monitor_collect_once(monitor_snapshot_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    out->free8 = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    out->min8  = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    out->lfb8  = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    out->free_int = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->min_int  = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->lfb_int  = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    UBaseType_t n = uxTaskGetNumberOfTasks();
    if (n == 0) return 0;

    TaskStatus_t *tasks = (TaskStatus_t *)calloc(n, sizeof(TaskStatus_t));
    if (!tasks) return -2;

    UBaseType_t got = uxTaskGetSystemState(tasks, n, NULL);

    uint32_t keep = (got > MONITOR_MAX_TASKS) ? MONITOR_MAX_TASKS : (uint32_t)got;
    out->task_count = keep;

    for (uint32_t i = 0; i < keep; i++) {
        if (tasks[i].pcTaskName) {
            strncpy(out->tasks[i].name, tasks[i].pcTaskName, MONITOR_TASK_NAME - 1);
            out->tasks[i].name[MONITOR_TASK_NAME - 1] = '\0';
        } else {
            strncpy(out->tasks[i].name, "?", MONITOR_TASK_NAME - 1);
        }

        out->tasks[i].prio = (uint8_t)tasks[i].uxCurrentPriority;
        out->tasks[i].state = (uint8_t)tasks[i].eCurrentState;

        uint32_t hwm_words = (uint32_t)tasks[i].usStackHighWaterMark;
        out->tasks[i].hwm_bytes = hwm_words * (uint32_t)sizeof(StackType_t);
    }

    free(tasks);
    return 0;
}

void monitor_log_once(void)
{
    monitor_snapshot_t s;
    int rc = monitor_collect_once(&s);
    if (rc != 0) {
        ESP_LOGW(TAG, "monitor_collect_once failed rc=%d", rc);
        return;
    }
    snapshot_log(&s);
}

// ---------------- periodic task ----------------

static void monitor_task(void *arg)
{
    int period_ms = (int)(intptr_t)arg;
    if (period_ms <= 0) period_ms = 10000;

    while (1) {
        monitor_log_once();
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }
}

void monitor_start(int period_ms)
{
    if (s_monitor_task) {
        ESP_LOGW(TAG, "monitor already running");
        return;
    }

    xTaskCreate(
        monitor_task,
        "monitor",
        4096,
        (void *)(intptr_t)period_ms,
        1,
        &s_monitor_task
    );

    ESP_LOGI(TAG, "monitor started");
}

void monitor_stop(void)
{
    if (!s_monitor_task) {
        ESP_LOGW(TAG, "monitor not running");
        return;
    }

    TaskHandle_t h = s_monitor_task;
    s_monitor_task = NULL;

    vTaskDelete(h);
    ESP_LOGI(TAG, "monitor stopped");
}
