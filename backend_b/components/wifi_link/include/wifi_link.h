#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    bool present;
    bool online;
    uint32_t last_seen_ms;
    uint16_t http_port;
    char ip[16];
} wifi_link_frontend_peer_info_t;

esp_err_t wifi_link_init(void);
esp_err_t wifi_link_send_packet(const uint8_t *data, size_t len);
esp_err_t wifi_link_get_local_ip_string(char *buffer, size_t buffer_size);
esp_err_t wifi_link_get_frontend_peer_info(wifi_link_frontend_peer_info_t *info);
