#pragma once

#include "esp_err.h"
#include "wdap_protocol.h"

esp_err_t uart_bridge_init(void);
esp_err_t uart_bridge_handle_message(const wdap_message_t *message);
