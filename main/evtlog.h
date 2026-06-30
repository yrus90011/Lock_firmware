#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    EVT_BOOT = 1,
    EVT_WS_DISCONNECT,
    EVT_NFC_FAIL,
    EVT_MPR_FAIL,
} evtlog_type_t;

typedef struct {
    uint32_t window_start_epoch;   // start of current 6h window (epoch)
    uint32_t boot_time_epoch;      // last boot time (epoch)
    uint32_t last_reset_reason;    // esp_reset_reason_t value

    uint32_t boot_count;           // counts within current 6h window
    uint32_t ws_disc_count;
    uint32_t nfc_fail_count;
    uint32_t mpr_fail_count;

    uint32_t last_update_epoch;    // last time we wrote/updated
} evtlog_state_t;

esp_err_t evtlog_init_and_start(void);     // call this FIRST in app_main()
void      evtlog_push(evtlog_type_t t);    // call from WS/NFC/MPR code
esp_err_t evtlog_get_state(evtlog_state_t *out); // optional for debugging/UI