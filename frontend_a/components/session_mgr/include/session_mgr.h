#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wdap_protocol.h"

esp_err_t session_mgr_init(void);
esp_err_t session_mgr_start(void);
bool session_mgr_is_ready(void);
esp_err_t session_mgr_send_command(uint8_t cmd,
                                   const void *payload,
                                   uint16_t payload_len,
                                   wdap_message_t *response,
                                   uint32_t timeout_ms);
void session_mgr_handle_incoming(const uint8_t *data, size_t len, void *ctx);
