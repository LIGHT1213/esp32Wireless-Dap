#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*wifi_link_rx_cb_t)(const uint8_t *data, size_t len, void *ctx);

esp_err_t wifi_link_init(wifi_link_rx_cb_t callback, void *ctx);
bool wifi_link_is_ready(void);
esp_err_t wifi_link_send_packet(const uint8_t *data, size_t len);
