#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WDAP_MAGIC                 0x57444150U
#define WDAP_VERSION               1U
#define WDAP_SESSION_ID            0x20260428U
#define WDAP_MAX_MESSAGE_SIZE      ((WDAP_DAP_PACKET_SIZE > WDAP_ESPNOW_PAYLOAD_MTU) ? WDAP_DAP_PACKET_SIZE : WDAP_ESPNOW_PAYLOAD_MTU)

typedef enum {
    WDAP_PKT_DAP_REQ = 1,
    WDAP_PKT_DAP_RSP,
    WDAP_PKT_DAP_REQ_NORESP,
    WDAP_PKT_DAP_BATCH_NORESP,
    WDAP_PKT_UART_DATA,
    WDAP_PKT_ACK,
    WDAP_PKT_NACK,
    WDAP_PKT_PING,
    WDAP_PKT_PONG,
    WDAP_PKT_STATS,
} wdap_pkt_type_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t header_len;
    uint32_t session_id;
    uint32_t msg_id;
    uint16_t seq;
    uint16_t ack_seq;
    uint16_t frag_idx;
    uint16_t frag_cnt;
    uint16_t payload_len;
    uint16_t flags;
    uint32_t crc32;
} wdap_hdr_t;

typedef struct {
    wdap_pkt_type_t type;
    uint32_t msg_id;
    size_t len;
    uint8_t payload[WDAP_MAX_MESSAGE_SIZE];
} wdap_message_t;

uint32_t wdap_protocol_crc32(const wdap_hdr_t *hdr, const uint8_t *payload);
bool wdap_protocol_validate(const wdap_hdr_t *hdr, const uint8_t *payload, size_t frame_len);
void wdap_protocol_format_mac(const uint8_t mac[6], char out[18]);

#ifdef __cplusplus
}
#endif
