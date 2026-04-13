#pragma once

#include "esp_err.h"

typedef void (*host_link_line_cb_t)(const char *line, void *ctx);

esp_err_t host_link_start(host_link_line_cb_t callback, void *ctx);
