#pragma once
#include "esp_err.h"

esp_err_t ws_upload_log_file(const char *fs_path, const char *remote_filename);

// Call this on every complete WS JSON RX message
void ws_log_upload_on_rx_json(const char *json_str);