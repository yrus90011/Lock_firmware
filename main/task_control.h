// task_control.h
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

void app_task_register(TaskHandle_t h, bool suspendable);
void app_tasks_suspend_all(void);
void app_tasks_resume_all(void);