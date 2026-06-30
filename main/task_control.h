#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_task_register(TaskHandle_t h);
void app_tasks_suspend_all(void);
void app_tasks_resume_all(void);