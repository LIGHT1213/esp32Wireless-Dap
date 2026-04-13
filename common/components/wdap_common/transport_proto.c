#include "transport_proto.h"

#include <string.h>

static uint16_t read_u16_le(const uint8_t *buffer)
{
    return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *buffer)
{
    return (uint32_t)buffer[0] |
           ((uint32_t)buffer[1] << 8) |
           ((uint32_t)buffer[2] << 16) |
           ((uint32_t)buffer[3] << 24);
}

static void write_u16_le(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value & 0xffU);
    buffer[1] = (uint8_t)((value >> 8) & 0xffU);
}

static void write_u32_le(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value & 0xffU);
    buffer[1] = (uint8_t)((value >> 8) & 0xffU);
    buffer[2] = (uint8_t)((value >> 16) & 0xffU);
    buffer[3] = (uint8_t)((value >> 24) & 0xffU);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *buffer, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        crc ^= buffer[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320UL & mask);
        }
    }
    return crc;
}

static uint32_t crc32_compute(const uint8_t *buffer, size_t size)
{
    return ~crc32_update(0xffffffffUL, buffer, size);
}

size_t transport_proto_encoded_size(uint16_t payload_len)
{
    return (size_t)payload_len + WDAP_PACKET_OVERHEAD;
}

esp_err_t transport_proto_encode(const wdap_message_t *message,
                                 uint8_t *buffer,
                                 size_t buffer_size,
                                 size_t *encoded_size)
{
    if (message == NULL || buffer == NULL || encoded_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (message->payload_len > WDAP_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t total_size = transport_proto_encoded_size(message->payload_len);
    if (buffer_size < total_size) {
        return ESP_ERR_NO_MEM;
    }

    write_u32_le(&buffer[0], WDAP_MAGIC);
    buffer[4] = WDAP_VERSION;
    buffer[5] = message->msg_type;
    buffer[6] = message->cmd;
    buffer[7] = message->status;
    write_u16_le(&buffer[8], message->seq);
    write_u16_le(&buffer[10], message->payload_len);
    buffer[12] = message->ack;
    buffer[13] = 0;
    buffer[14] = 0;
    buffer[15] = 0;

    if (message->payload_len > 0U) {
        memcpy(&buffer[WDAP_PACKET_HEADER_SIZE], message->payload, message->payload_len);
    }

    const uint32_t crc = crc32_compute(buffer, WDAP_PACKET_HEADER_SIZE + message->payload_len);
    write_u32_le(&buffer[WDAP_PACKET_HEADER_SIZE + message->payload_len], crc);
    *encoded_size = total_size;
    return ESP_OK;
}

esp_err_t transport_proto_decode(const uint8_t *buffer,
                                 size_t buffer_size,
                                 wdap_message_t *message)
{
    if (buffer == NULL || message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (buffer_size < WDAP_PACKET_OVERHEAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (read_u32_le(&buffer[0]) != WDAP_MAGIC) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (buffer[4] != WDAP_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }

    const uint16_t payload_len = read_u16_le(&buffer[10]);
    if (payload_len > WDAP_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t expected_size = transport_proto_encoded_size(payload_len);
    if (buffer_size != expected_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t expected_crc = read_u32_le(&buffer[WDAP_PACKET_HEADER_SIZE + payload_len]);
    const uint32_t actual_crc = crc32_compute(buffer, WDAP_PACKET_HEADER_SIZE + payload_len);
    if (expected_crc != actual_crc) {
        return ESP_ERR_INVALID_CRC;
    }

    memset(message, 0, sizeof(*message));
    message->msg_type = buffer[5];
    message->cmd = buffer[6];
    message->status = buffer[7];
    message->seq = read_u16_le(&buffer[8]);
    message->payload_len = payload_len;
    message->ack = buffer[12];

    if (payload_len > 0U) {
        memcpy(message->payload, &buffer[WDAP_PACKET_HEADER_SIZE], payload_len);
    }

    return ESP_OK;
}

const char *wdap_cmd_to_string(uint8_t cmd)
{
    switch (cmd) {
    case WDAP_CMD_PING:
        return "PING";
    case WDAP_CMD_GET_VERSION:
        return "GET_VERSION";
    case WDAP_CMD_GET_CAPS:
        return "GET_CAPS";
    case WDAP_CMD_SET_SWD_FREQ:
        return "SET_SWD_FREQ";
    case WDAP_CMD_SWD_LINE_RESET:
        return "SWD_LINE_RESET";
    case WDAP_CMD_TARGET_RESET:
        return "TARGET_RESET";
    case WDAP_CMD_READ_DP_IDCODE:
        return "READ_DP_IDCODE";
    case WDAP_CMD_SWD_READ_DP:
        return "SWD_READ_DP";
    case WDAP_CMD_SWD_WRITE_DP:
        return "SWD_WRITE_DP";
    case WDAP_CMD_SWD_READ_AP:
        return "SWD_READ_AP";
    case WDAP_CMD_SWD_WRITE_AP:
        return "SWD_WRITE_AP";
    case WDAP_CMD_SWD_READ_BLOCK:
        return "SWD_READ_BLOCK";
    case WDAP_CMD_SWD_WRITE_BLOCK:
        return "SWD_WRITE_BLOCK";
    case WDAP_CMD_TARGET_HALT:
        return "TARGET_HALT";
    default:
        return "UNKNOWN";
    }
}

const char *wdap_status_to_string(uint8_t status)
{
    switch (status) {
    case WDAP_STATUS_OK:
        return "OK";
    case WDAP_STATUS_BAD_CRC:
        return "BAD_CRC";
    case WDAP_STATUS_BAD_FRAME:
        return "BAD_FRAME";
    case WDAP_STATUS_BAD_PAYLOAD:
        return "BAD_PAYLOAD";
    case WDAP_STATUS_TIMEOUT:
        return "TIMEOUT";
    case WDAP_STATUS_BUSY:
        return "BUSY";
    case WDAP_STATUS_NOT_READY:
        return "NOT_READY";
    case WDAP_STATUS_UNSUPPORTED:
        return "UNSUPPORTED";
    case WDAP_STATUS_INTERNAL_ERROR:
        return "INTERNAL_ERROR";
    case WDAP_STATUS_NO_TARGET:
        return "NO_TARGET";
    default:
        return "UNKNOWN";
    }
}
