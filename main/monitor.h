#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MONITOR_MAX_TASKS 20
#define MONITOR_TASK_NAME 16

typedef struct {
    char name[MONITOR_TASK_NAME];
    uint8_t prio;
    uint8_t state;
    uint32_t hwm_bytes;
} monitor_task_t;

typedef struct {
    uint32_t free8, min8, lfb8;
    uint32_t free_int, min_int, lfb_int;

    uint32_t task_count;
    monitor_task_t tasks[MONITOR_MAX_TASKS];
} monitor_snapshot_t;

// Collect heap+tasks one time (no logging)
int monitor_collect_once(monitor_snapshot_t *out);

// Log heap+tasks one time using ESP_LOGI
void monitor_log_once(void);

// Periodic logger task
void monitor_start(int period_ms);
void monitor_stop(void);

#ifdef __cplusplus
}
#endif