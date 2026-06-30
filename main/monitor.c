#include "monitor.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

static const char *TAG = "MON";

static void log_heap_stats(void)
{
    size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t min8  = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    size_t lfb8  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t min_int  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t lfb_int  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    ESP_LOGI(TAG, "HEAP 8BIT: free=%u min=%u lfb=%u", (unsigned)free8, (unsigned)min8, (unsigned)lfb8);
    ESP_LOGI(TAG, "HEAP INT : free=%u min=%u lfb=%u", (unsigned)free_int, (unsigned)min_int, (unsigned)lfb_int);
}

static void log_task_stacks(void)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = (TaskStatus_t *)calloc(n, sizeof(TaskStatus_t));
    if (!tasks) {
        ESP_LOGW(TAG, "No mem to list tasks");
        return;
    }

    UBaseType_t got = uxTaskGetSystemState(tasks, n, NULL);
    ESP_LOGI(TAG, "TASK STACKS (high water = min remaining) count=%u", (unsigned)got);

    for (UBaseType_t i = 0; i < got; i++) {
        uint32_t hwm_bytes = (uint32_t)tasks[i].usStackHighWaterMark * (uint32_t)sizeof(StackType_t);
        ESP_LOGI(TAG, "  %-16s prio=%u hwm=%u bytes state=%u",
                 tasks[i].pcTaskName,
                 (unsigned)tasks[i].uxCurrentPriority,
                 (unsigned)hwm_bytes,
                 (unsigned)tasks[i].eCurrentState);
    }

    free(tasks);
}

static void monitor_task(void *arg)
{
    const int period_ms = 10000;
    while (1) {
        log_heap_stats();
        log_task_stacks();
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }
}

void monitor_start(void)
{
    xTaskCreate(monitor_task, "monitor", 4096, NULL, 1, NULL);
}