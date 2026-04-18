#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t wdap_runtime_init(const char *role);
const char *wdap_runtime_get_role(void);
const char *wdap_runtime_get_version(void);
const char *wdap_runtime_get_busy_reason(void);
bool wdap_runtime_is_busy(void);
esp_err_t wdap_runtime_try_acquire_busy(const char *reason);
void wdap_runtime_release_busy(void);
esp_err_t wdap_runtime_get_running_partition_label(char *buffer, size_t buffer_size);
esp_err_t wdap_runtime_mark_running_partition_valid(void);
