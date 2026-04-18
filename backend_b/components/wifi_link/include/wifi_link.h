#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t wifi_link_init(void);
esp_err_t wifi_link_send_packet(const uint8_t *data, size_t len);
