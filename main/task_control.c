#include "task_control.h"
#include "esp_log.h"

static const char *TAG="TASKCTL";

typedef struct {
    TaskHandle_t h;
    bool suspendable;
} app_task_entry_t;

static app_task_entry_t s_tasks[12];
static int s_count = 0;

void app_task_register(TaskHandle_t h, bool suspendable)
{
    if (!h) return;
    if (s_count >= (int)(sizeof(s_tasks)/sizeof(s_tasks[0]))) return;

    // prevent duplicates
    for (int i = 0; i < s_count; i++) {
        if (s_tasks[i].h == h) return;
    }

    s_tasks[s_count++] = (app_task_entry_t){ .h = h, .suspendable = suspendable };
}

void app_tasks_suspend_all(void)
{
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        if (s_tasks[i].h && s_tasks[i].suspendable) {
            vTaskSuspend(s_tasks[i].h);
            n++;
        }
    }
    ESP_LOGW(TAG, "Suspended %d app tasks (of %d registered)", n, s_count);
}

void app_tasks_resume_all(void)
{
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        if (s_tasks[i].h && s_tasks[i].suspendable) {
            vTaskResume(s_tasks[i].h);
            n++;
        }
    }
    ESP_LOGW(TAG, "Resumed %d app tasks (of %d registered)", n, s_count);
}