#pragma once

#include "esp_err.h"

esp_err_t dap_frontend_init(void);
void dap_frontend_handle_host_line(const char *line, void *ctx);
