#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wdap_protocol.h"

size_t transport_proto_encoded_size(uint16_t payload_len);
esp_err_t transport_proto_encode(const wdap_message_t *message,
                                 uint8_t *buffer,
                                 size_t buffer_size,
                                 size_t *encoded_size);
esp_err_t transport_proto_decode(const uint8_t *buffer,
                                 size_t buffer_size,
                                 wdap_message_t *message);
