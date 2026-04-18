#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef esp_err_t (*wdap_http_ota_get_ip_fn_t)(char *buffer, size_t buffer_size);
typedef esp_err_t (*wdap_http_ota_build_devices_json_fn_t)(char *buffer, size_t buffer_size, size_t *written_len);

typedef struct {
    wdap_http_ota_get_ip_fn_t get_ip;
    const char *index_html;
    wdap_http_ota_build_devices_json_fn_t build_devices_json;
} wdap_http_ota_config_t;

esp_err_t wdap_http_ota_start(const wdap_http_ota_config_t *config);
