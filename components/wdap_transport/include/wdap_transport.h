#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "wdap_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t tx_messages;
    uint32_t rx_messages;
    uint32_t ack_rx;
    uint32_t ack_tx;
    uint32_t nack_rx;
    uint32_t nack_tx;
    uint32_t retries;
    uint32_t timeouts;
    uint32_t crc_errors;
    uint32_t duplicates;
    uint32_t reassembly_errors;
    uint32_t send_failures;
} wdap_transport_stats_t;

esp_err_t wdap_transport_init(void);
esp_err_t wdap_transport_send_message(wdap_pkt_type_t type, const uint8_t *payload, size_t len, uint32_t timeout_ms);
esp_err_t wdap_transport_recv_message(wdap_message_t *message, uint32_t timeout_ms);
void wdap_transport_get_stats(wdap_transport_stats_t *stats);
void wdap_transport_log_stats(void);

#ifdef __cplusplus
}
#endif
