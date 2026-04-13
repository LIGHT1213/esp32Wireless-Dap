#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t dap_backend_process_frame(const uint8_t *rx_data,
                                    size_t rx_len,
                                    uint8_t *tx_data,
                                    size_t tx_capacity,
                                    size_t *tx_len);
