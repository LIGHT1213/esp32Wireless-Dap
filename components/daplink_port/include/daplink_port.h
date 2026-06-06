#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t daplink_port_init(void);
size_t daplink_port_process(const uint8_t *request, size_t request_len, uint8_t *response, size_t response_capacity);

#ifdef __cplusplus
}
#endif
