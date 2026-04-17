#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WDAP_MAGIC 0x50414457UL
#define WDAP_VERSION 0x01U
#define WDAP_MAX_PAYLOAD 256U
#define WDAP_PACKET_HEADER_SIZE 16U
#define WDAP_PACKET_CRC_SIZE 4U
#define WDAP_PACKET_OVERHEAD (WDAP_PACKET_HEADER_SIZE + WDAP_PACKET_CRC_SIZE)
#define WDAP_MAX_FRAME_SIZE (WDAP_MAX_PAYLOAD + WDAP_PACKET_OVERHEAD)

typedef enum {
    WDAP_MSG_REQUEST = 0x01,
    WDAP_MSG_RESPONSE = 0x02,
    WDAP_MSG_HEARTBEAT = 0x03,
} wdap_msg_type_t;

typedef enum {
    WDAP_CMD_PING = 0x01,
    WDAP_CMD_GET_VERSION = 0x02,
    WDAP_CMD_GET_CAPS = 0x03,
    WDAP_CMD_SET_SWD_FREQ = 0x10,
    WDAP_CMD_SWD_LINE_RESET = 0x11,
    WDAP_CMD_TARGET_RESET = 0x12,
    WDAP_CMD_READ_DP_IDCODE = 0x13,
    WDAP_CMD_SWD_READ_DP = 0x14,
    WDAP_CMD_SWD_WRITE_DP = 0x15,
    WDAP_CMD_SWD_READ_AP = 0x16,
    WDAP_CMD_SWD_WRITE_AP = 0x17,
    WDAP_CMD_SWD_READ_BLOCK = 0x18,
    WDAP_CMD_SWD_WRITE_BLOCK = 0x19,
    WDAP_CMD_TARGET_HALT = 0x1A,
    WDAP_CMD_TARGET_RESET_DRIVE = 0x1B,
    WDAP_CMD_SET_TRANSFER_CONFIG = 0x1C,
    WDAP_CMD_SWD_TRANSFER_SEQUENCE = 0x1D,
    WDAP_CMD_SWJ_SEQUENCE = 0x1E,
} wdap_cmd_t;

typedef enum {
    WDAP_STATUS_OK = 0,
    WDAP_STATUS_BAD_CRC = 1,
    WDAP_STATUS_BAD_FRAME = 2,
    WDAP_STATUS_BAD_PAYLOAD = 3,
    WDAP_STATUS_TIMEOUT = 4,
    WDAP_STATUS_BUSY = 5,
    WDAP_STATUS_NOT_READY = 6,
    WDAP_STATUS_UNSUPPORTED = 7,
    WDAP_STATUS_INTERNAL_ERROR = 8,
    WDAP_STATUS_NO_TARGET = 9,
} wdap_status_t;

typedef enum {
    WDAP_ACK_NONE = 0x00,
    WDAP_ACK_OK = 0x01,
    WDAP_ACK_WAIT = 0x02,
    WDAP_ACK_FAULT = 0x04,
    WDAP_ACK_PARITY = 0x08,
    WDAP_ACK_PROTOCOL = 0x10,
} wdap_ack_t;

typedef enum {
    WDAP_CAP_PING = 1UL << 0,
    WDAP_CAP_LINE_RESET = 1UL << 1,
    WDAP_CAP_READ_DP = 1UL << 2,
    WDAP_CAP_WRITE_DP = 1UL << 3,
    WDAP_CAP_TARGET_RESET = 1UL << 4,
    WDAP_CAP_HEARTBEAT = 1UL << 5,
    WDAP_CAP_MOCK_SWD = 1UL << 6,
    WDAP_CAP_AP_ACCESS = 1UL << 7,
    WDAP_CAP_TARGET_HALT = 1UL << 8,
} wdap_cap_flags_t;

typedef struct {
    uint8_t msg_type;
    uint8_t cmd;
    uint8_t status;
    uint8_t ack;
    uint16_t seq;
    uint16_t payload_len;
    uint8_t payload[WDAP_MAX_PAYLOAD];
} wdap_message_t;

typedef struct {
    uint32_t nonce;
} wdap_ping_request_t;

typedef struct {
    uint32_t nonce;
    uint32_t uptime_ms;
} wdap_ping_response_t;

typedef struct {
    uint32_t feature_flags;
    uint32_t max_payload;
    uint32_t default_swd_hz;
    uint32_t current_swd_hz;
} wdap_caps_response_t;

typedef struct {
    uint32_t hz;
} wdap_set_swd_freq_request_t;

typedef struct {
    uint8_t addr;
} wdap_reg_read_request_t;

typedef struct {
    uint8_t addr;
    uint32_t value;
} wdap_reg_write_request_t;

typedef struct {
    uint32_t value;
} wdap_reg_value_response_t;

typedef struct {
    uint8_t asserted;
} wdap_target_reset_drive_request_t;

typedef struct {
    uint8_t idle_cycles;
    uint8_t turnaround;
    uint8_t data_phase;
    uint8_t reserved;
    uint16_t retry_count;
    uint16_t match_retry;
} wdap_transfer_config_request_t;

typedef struct {
    uint8_t count;
    uint8_t reserved0;
    uint16_t retry_count;
    uint16_t match_retry;
    uint32_t match_mask;
    /* followed by count entries:
     *   uint8_t request_value
     *   uint8_t resolved_addr
     *   if read + MATCH_VALUE: uint32_t match_value_le
     *   if write: uint32_t value_le
     */
} wdap_transfer_sequence_request_t;

typedef struct {
    uint8_t completed;
    uint8_t result_flags;
    uint8_t reserved[2];
    /* followed by read data for successful reads in order */
} wdap_transfer_sequence_response_t;

typedef struct {
    uint8_t flags;      /* bit 0: APnDP, bits 2-3: A[3:2] */
    uint8_t count_lo;   /* transfer count low byte */
    uint8_t count_hi;   /* transfer count high byte */
    /* For writes: followed by count * uint32_t values (little-endian) */
} wdap_block_request_t;

typedef struct {
    uint8_t ack;            /* SWD ack from last transfer */
    uint8_t completed_lo;   /* completed count low byte */
    uint8_t completed_hi;   /* completed count high byte */
    /* For reads: followed by completed * uint32_t values (little-endian) */
} wdap_block_response_t;

const char *wdap_cmd_to_string(uint8_t cmd);
const char *wdap_status_to_string(uint8_t status);
