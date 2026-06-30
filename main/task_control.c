#include "task_control.h"
#include "esp_log.h"

static const char *TAG="TASKCTL";

// store only YOUR task handles
static TaskHandle_t s_tasks[12];
static int s_count = 0;

void app_task_register(TaskHandle_t h)
{
    if (!h) return;
    if (s_count >= (int)(sizeof(s_tasks)/sizeof(s_tasks[0]))) return;
    s_tasks[s_count++] = h;
}

void app_tasks_suspend_all(void)
{
    for (int i = 0; i < s_count; i++) {
        if (s_tasks[i]) vTaskSuspend(s_tasks[i]);
    }
    ESP_LOGW(TAG, "Suspended %d app tasks", s_count);
}

void app_tasks_resume_all(void)
{
    for (int i = 0; i < s_count; i++) {
        if (s_tasks[i]) vTaskResume(s_tasks[i]);
    }
    ESP_LOGW(TAG, "Resumed %d app tasks", s_count);
}
