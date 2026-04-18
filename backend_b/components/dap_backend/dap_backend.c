#include "dap_backend.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "log_utils.h"
#include "swd_engine.h"
#include "transport_proto.h"
#include "wdap_runtime.h"

static const char *TAG = "dap_backend";

#define DAP_TRANSFER_OK BIT(0)
#define DAP_TRANSFER_WAIT BIT(1)
#define DAP_TRANSFER_FAULT BIT(2)
#define DAP_TRANSFER_ERROR BIT(3)
#define DAP_TRANSFER_MISMATCH BIT(4)
#define DAP_OK 0x00U

typedef struct {
    uint16_t retry_count;
    uint16_t match_retry;
} dap_transfer_settings_t;

static dap_transfer_settings_t s_transfer_settings = {
    .retry_count = 5,
    .match_retry = 5,
};

typedef esp_err_t (*dap_read_fn_t)(uint8_t addr, uint32_t *value, uint8_t *ack);
typedef esp_err_t (*dap_write_fn_t)(uint8_t addr, uint32_t value, uint8_t *ack);

static esp_err_t read_dp_idcode_adapter(uint8_t addr, uint32_t *value, uint8_t *ack)
{
    (void)addr;
    return swd_engine_read_dp_idcode(value, ack);
}

static uint32_t read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static void write_u32_le(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 0);
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static uint8_t status_from_err(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return WDAP_STATUS_OK;
    case ESP_ERR_INVALID_ARG:
        return WDAP_STATUS_BAD_PAYLOAD;
    case ESP_ERR_INVALID_STATE:
        return WDAP_STATUS_NOT_READY;
    case ESP_ERR_TIMEOUT:
        return WDAP_STATUS_TIMEOUT;
    case ESP_ERR_NOT_SUPPORTED:
        return WDAP_STATUS_UNSUPPORTED;
    case ESP_ERR_INVALID_CRC:
        return WDAP_STATUS_INTERNAL_ERROR;
    default:
        return WDAP_STATUS_INTERNAL_ERROR;
    }
}

static uint8_t wdap_ack_to_dap_flags(uint8_t ack)
{
    if ((ack & WDAP_ACK_WAIT) != 0U) {
        return DAP_TRANSFER_WAIT;
    }
    if ((ack & WDAP_ACK_FAULT) != 0U) {
        return DAP_TRANSFER_FAULT;
    }
    if ((ack & WDAP_ACK_OK) != 0U) {
        return DAP_TRANSFER_OK;
    }
    return DAP_TRANSFER_ERROR;
}

static esp_err_t append_payload(wdap_message_t *response, const void *payload, size_t payload_len)
{
    if (payload_len > WDAP_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    response->payload_len = (uint16_t)payload_len;
    if (payload_len > 0U && payload != NULL) {
        memcpy(response->payload, payload, payload_len);
    }
    return ESP_OK;
}

static esp_err_t handle_ping(const wdap_message_t *request, wdap_message_t *response)
{
    wdap_ping_response_t payload = {
        .nonce = 0,
        .uptime_ms = log_utils_uptime_ms(),
    };

    if (request->payload_len >= sizeof(wdap_ping_request_t)) {
        const wdap_ping_request_t *ping = (const wdap_ping_request_t *)request->payload;
        payload.nonce = ping->nonce;
    }

    return append_payload(response, &payload, sizeof(payload));
}

static esp_err_t execute_read_with_retry(dap_read_fn_t fn,
                                         uint8_t addr,
                                         uint32_t *value,
                                         uint8_t *ack,
                                         uint16_t retry_count)
{
    uint8_t local_ack = WDAP_ACK_NONE;
    esp_err_t err = ESP_FAIL;

    for (uint16_t attempt = 0; attempt <= retry_count; ++attempt) {
        err = fn(addr, value, &local_ack);
        if (local_ack != WDAP_ACK_WAIT && err != ESP_ERR_TIMEOUT) {
            break;
        }
    }

    if (ack != NULL) {
        *ack = local_ack;
    }
    return err;
}

static esp_err_t execute_write_with_retry(dap_write_fn_t fn,
                                          uint8_t addr,
                                          uint32_t value,
                                          uint8_t *ack,
                                          uint16_t retry_count)
{
    uint8_t local_ack = WDAP_ACK_NONE;
    esp_err_t err = ESP_FAIL;

    for (uint16_t attempt = 0; attempt <= retry_count; ++attempt) {
        err = fn(addr, value, &local_ack);
        if (local_ack != WDAP_ACK_WAIT && err != ESP_ERR_TIMEOUT) {
            break;
        }
    }

    if (ack != NULL) {
        *ack = local_ack;
    }
    return err;
}

static esp_err_t handle_get_version(wdap_message_t *response)
{
    char version[96];
    const int written = snprintf(version,
                                 sizeof(version),
                                 "backend-b idf=%s mode=%s",
                                 esp_get_idf_version(),
                                 swd_engine_is_mock_mode() ? "mock" : "hardware");
    if (written < 0) {
        return ESP_FAIL;
    }

    return append_payload(response, version, (size_t)written);
}

static esp_err_t handle_get_caps(wdap_message_t *response)
{
    const wdap_caps_response_t payload = {
        .feature_flags = swd_engine_get_capabilities() | WDAP_CAP_UART_BRIDGE,
        .max_payload = WDAP_MAX_PAYLOAD,
        .default_swd_hz = swd_engine_get_default_frequency(),
        .current_swd_hz = swd_engine_get_current_frequency(),
    };
    return append_payload(response, &payload, sizeof(payload));
}

static esp_err_t handle_set_swd_freq(const wdap_message_t *request, wdap_message_t *response)
{
    if (request->payload_len < sizeof(wdap_set_swd_freq_request_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    const wdap_set_swd_freq_request_t *cfg = (const wdap_set_swd_freq_request_t *)request->payload;
    ESP_RETURN_ON_ERROR(swd_engine_set_frequency(cfg->hz), TAG, "set swd freq failed");

    const wdap_reg_value_response_t payload = {
        .value = swd_engine_get_current_frequency(),
    };
    return append_payload(response, &payload, sizeof(payload));
}

static esp_err_t handle_line_reset(wdap_message_t *response)
{
    uint8_t ack = WDAP_ACK_NONE;
    const esp_err_t err = swd_engine_line_reset(&ack);
    response->ack = ack;
    return err;
}

static esp_err_t handle_target_reset(wdap_message_t *response)
{
    response->ack = WDAP_ACK_OK;
    return swd_engine_target_reset();
}

static esp_err_t handle_target_reset_drive(const wdap_message_t *request, wdap_message_t *response)
{
    if (request->payload_len < sizeof(wdap_target_reset_drive_request_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    const wdap_target_reset_drive_request_t *drive = (const wdap_target_reset_drive_request_t *)request->payload;
    response->ack = WDAP_ACK_OK;
    return swd_engine_target_reset_drive(drive->asserted != 0U);
}

static esp_err_t handle_swj_pins(const wdap_message_t *request, wdap_message_t *response)
{
    if (request->payload_len < sizeof(wdap_swj_pins_request_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    const wdap_swj_pins_request_t *pins_req = (const wdap_swj_pins_request_t *)request->payload;
    wdap_swj_pins_response_t pins_resp = {0};
    ESP_RETURN_ON_ERROR(swd_engine_swj_pins(pins_req->value,
                                            pins_req->select,
                                            pins_req->wait_us,
                                            &pins_resp.pins),
                        TAG,
                        "swj pins failed");
    response->ack = WDAP_ACK_OK;
    return append_payload(response, &pins_resp, sizeof(pins_resp));
}

static esp_err_t handle_swd_sequence(const wdap_message_t *request, wdap_message_t *response)
{
    if (request->payload_len < 1U) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *cursor = request->payload;
    const uint8_t *end = request->payload + request->payload_len;
    const uint8_t sequence_count = *cursor++;
    uint8_t resp_payload[WDAP_MAX_PAYLOAD] = {0};
    uint8_t *dst = resp_payload;

    *dst++ = DAP_OK;

    for (uint8_t i = 0; i < sequence_count; ++i) {
        if (cursor >= end) {
            return ESP_ERR_INVALID_ARG;
        }

        const uint8_t info = *cursor++;
        uint32_t count_bits = info & 0x3FU;
        if (count_bits == 0U) {
            count_bits = 64U;
        }
        const uint32_t count_bytes = (count_bits + 7U) / 8U;
        const bool capture = (info & 0x80U) != 0U;

        if (capture) {
            if ((size_t)(&resp_payload[WDAP_MAX_PAYLOAD] - dst) < count_bytes) {
                return ESP_ERR_INVALID_SIZE;
            }
            ESP_RETURN_ON_ERROR(swd_engine_swd_sequence(info, NULL, dst), TAG, "swd capture sequence failed");
            dst += count_bytes;
        } else {
            if ((size_t)(end - cursor) < count_bytes) {
                return ESP_ERR_INVALID_ARG;
            }
            ESP_RETURN_ON_ERROR(swd_engine_swd_sequence(info, cursor, NULL), TAG, "swd output sequence failed");
            cursor += count_bytes;
        }
    }

    response->ack = WDAP_ACK_OK;
    return append_payload(response, resp_payload, (size_t)(dst - resp_payload));
}

static esp_err_t handle_transfer_config(const wdap_message_t *request, wdap_message_t *response)
{
    if (request->payload_len < sizeof(wdap_transfer_config_request_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    const wdap_transfer_config_request_t *cfg = (const wdap_transfer_config_request_t *)request->payload;
    s_transfer_settings.retry_count = cfg->retry_count;
    s_transfer_settings.match_retry = cfg->match_retry;
    response->ack = WDAP_ACK_OK;
    return swd_engine_set_transfer_config(cfg->idle_cycles, cfg->turnaround, cfg->data_phase);
}

static esp_err_t handle_target_halt(wdap_message_t *response)
{
    wdap_reg_value_response_t payload = {0};
    response->ack = WDAP_ACK_OK;
    ESP_RETURN_ON_ERROR(swd_engine_target_halt(&payload.value), TAG, "target halt failed");
    return append_payload(response, &payload, sizeof(payload));
}

static esp_err_t handle_read_dp(uint8_t addr, wdap_message_t *response)
{
    uint8_t ack = WDAP_ACK_NONE;
    wdap_reg_value_response_t payload = {0};
    const esp_err_t err = (addr == 0x00U)
                              ? execute_read_with_retry(read_dp_idcode_adapter,
                                                        addr,
                                                        &payload.value,
                                                        &ack,
                                                        s_transfer_settings.retry_count)
                              : execute_read_with_retry(swd_engine_read_dp,
                                                        addr,
                                                        &payload.value,
                                                        &ack,
                                                        s_transfer_settings.retry_count);
    response->ack = ack;
    if (err != ESP_OK) {
        return err;
    }

    return append_payload(response, &payload, sizeof(payload));
}

static esp_err_t handle_write_dp(const wdap_message_t *request, wdap_message_t *response)
{
    if (request->payload_len < sizeof(wdap_reg_write_request_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    const wdap_reg_write_request_t *write = (const wdap_reg_write_request_t *)request->payload;
    uint8_t ack = WDAP_ACK_NONE;
    const esp_err_t err = execute_write_with_retry(swd_engine_write_dp,
                                                   write->addr,
                                                   write->value,
                                                   &ack,
                                                   s_transfer_settings.retry_count);
    response->ack = ack;
    return err;
}

static esp_err_t handle_read_ap(uint8_t addr, wdap_message_t *response)
{
    uint8_t ack = WDAP_ACK_NONE;
    wdap_reg_value_response_t payload = {0};
    const esp_err_t err = execute_read_with_retry(swd_engine_read_ap,
                                                  addr,
                                                  &payload.value,
                                                  &ack,
                                                  s_transfer_settings.retry_count);
    response->ack = ack;
    if (err != ESP_OK) {
        return err;
    }

    return append_payload(response, &payload, sizeof(payload));
}

static esp_err_t handle_write_ap(const wdap_message_t *request, wdap_message_t *response)
{
    if (request->payload_len < sizeof(wdap_reg_write_request_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    const wdap_reg_write_request_t *write = (const wdap_reg_write_request_t *)request->payload;
    uint8_t ack = WDAP_ACK_NONE;
    const esp_err_t err = execute_write_with_retry(swd_engine_write_ap,
                                                   write->addr,
                                                   write->value,
                                                   &ack,
                                                   s_transfer_settings.retry_count);
    response->ack = ack;
    return err;
}

static esp_err_t handle_write_block(const wdap_message_t *request, wdap_message_t *response)
{
    if (request->payload_len < sizeof(wdap_block_request_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    const wdap_block_request_t *hdr = (const wdap_block_request_t *)request->payload;
    const bool apndp = (hdr->flags & 0x01U) != 0U;
    const uint8_t addr = hdr->flags & 0x0CU;
    const uint16_t count = (uint16_t)hdr->count_lo | ((uint16_t)hdr->count_hi << 8);

    if (request->payload_len < sizeof(wdap_block_request_t) + (size_t)count * sizeof(uint32_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *values_data = &request->payload[sizeof(wdap_block_request_t)];
    uint16_t completed = 0;
    uint8_t ack = WDAP_ACK_OK;

    for (uint16_t i = 0; i < count; ++i) {
        const uint32_t value = read_u32_le(&values_data[i * sizeof(uint32_t)]);
        uint8_t transfer_ack = WDAP_ACK_NONE;
        esp_err_t err;
        if (apndp) {
            err = execute_write_with_retry(swd_engine_write_ap,
                                           addr,
                                           value,
                                           &transfer_ack,
                                           s_transfer_settings.retry_count);
        } else {
            err = execute_write_with_retry(swd_engine_write_dp,
                                           addr,
                                           value,
                                           &transfer_ack,
                                           s_transfer_settings.retry_count);
        }
        if (transfer_ack == WDAP_ACK_NONE) {
            ack = WDAP_ACK_FAULT;
            break;
        }
        ack = transfer_ack;
        if (err != ESP_OK || ack != WDAP_ACK_OK) {
            break;
        }
        ++completed;
    }

    response->ack = ack;
    wdap_block_response_t resp = {
        .ack = ack,
        .completed_lo = (uint8_t)(completed >> 0),
        .completed_hi = (uint8_t)(completed >> 8),
    };
    return append_payload(response, &resp, sizeof(resp));
}

static esp_err_t handle_read_block(const wdap_message_t *request, wdap_message_t *response)
{
    if (request->payload_len < sizeof(wdap_block_request_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    const wdap_block_request_t *hdr = (const wdap_block_request_t *)request->payload;
    const bool apndp = (hdr->flags & 0x01U) != 0U;
    const uint8_t addr = hdr->flags & 0x0CU;
    const uint16_t count = (uint16_t)hdr->count_lo | ((uint16_t)hdr->count_hi << 8);

    const size_t max_count = (WDAP_MAX_PAYLOAD - sizeof(wdap_block_response_t)) / sizeof(uint32_t);
    const uint16_t limited_count = (count > max_count) ? (uint16_t)max_count : count;

    uint8_t resp_payload[WDAP_MAX_PAYLOAD];
    uint16_t completed = 0;
    uint8_t ack = WDAP_ACK_OK;

    for (uint16_t i = 0; i < limited_count; ++i) {
        uint32_t value = 0;
        uint8_t transfer_ack = WDAP_ACK_NONE;
        esp_err_t err;
        if (apndp) {
            err = execute_read_with_retry(swd_engine_read_ap,
                                          addr,
                                          &value,
                                          &transfer_ack,
                                          s_transfer_settings.retry_count);
        } else {
            err = execute_read_with_retry(swd_engine_read_dp,
                                          addr,
                                          &value,
                                          &transfer_ack,
                                          s_transfer_settings.retry_count);
        }
        if (transfer_ack == WDAP_ACK_NONE) {
            ack = WDAP_ACK_FAULT;
            break;
        }
        ack = transfer_ack;
        if (err != ESP_OK || ack != WDAP_ACK_OK) {
            break;
        }
        write_u32_le(&resp_payload[sizeof(wdap_block_response_t) + i * sizeof(uint32_t)], value);
        ++completed;
    }

    response->ack = ack;
    wdap_block_response_t *resp_hdr = (wdap_block_response_t *)resp_payload;
    resp_hdr->ack = ack;
    resp_hdr->completed_lo = (uint8_t)(completed >> 0);
    resp_hdr->completed_hi = (uint8_t)(completed >> 8);

    return append_payload(response, resp_payload,
                          sizeof(wdap_block_response_t) + (size_t)completed * sizeof(uint32_t));
}

static esp_err_t handle_transfer_sequence(const wdap_message_t *request, wdap_message_t *response)
{
    if (request->payload_len < sizeof(wdap_transfer_sequence_request_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    const wdap_transfer_sequence_request_t *hdr = (const wdap_transfer_sequence_request_t *)request->payload;
    const uint8_t count = hdr->count;
    const uint8_t *cursor = &request->payload[sizeof(wdap_transfer_sequence_request_t)];
    const uint8_t *end = &request->payload[request->payload_len];
    uint8_t resp_payload[WDAP_MAX_PAYLOAD] = {0};
    uint8_t *dst = &resp_payload[sizeof(wdap_transfer_sequence_response_t)];
    uint8_t completed = 0;
    uint8_t ack = WDAP_ACK_OK;
    uint8_t result_flags = DAP_TRANSFER_OK;
    uint32_t match_mask = hdr->match_mask;
    bool check_write = false;

    for (uint8_t i = 0; i < count; ++i) {
        if ((size_t)(end - cursor) < 2U) {
            return ESP_ERR_INVALID_ARG;
        }

        const uint8_t request_value = *cursor++;
        const uint8_t addr = *cursor++;
        const bool apndp = (request_value & 0x01U) != 0U;
        const bool read = (request_value & 0x02U) != 0U;
        const bool use_match = (request_value & 0x10U) != 0U;
        const bool write_match_mask = (!read) && ((request_value & 0x20U) != 0U);
        uint8_t transfer_ack = WDAP_ACK_OK;
        esp_err_t err = ESP_OK;

        if (read) {
            check_write = false;
            uint32_t value = 0;
            uint32_t match_value = 0;
            if (use_match) {
                if ((size_t)(end - cursor) < sizeof(uint32_t)) {
                    return ESP_ERR_INVALID_ARG;
                }
                match_value = read_u32_le(cursor);
                cursor += sizeof(uint32_t);
            }

            uint16_t retries = hdr->match_retry;
            do {
                if (apndp) {
                    err = execute_read_with_retry(swd_engine_read_ap,
                                                  addr,
                                                  &value,
                                                  &transfer_ack,
                                                  hdr->retry_count);
                } else if (addr == 0x00U) {
                    err = execute_read_with_retry(read_dp_idcode_adapter,
                                                  addr,
                                                  &value,
                                                  &transfer_ack,
                                                  hdr->retry_count);
                } else {
                    err = execute_read_with_retry(swd_engine_read_dp,
                                                  addr,
                                                  &value,
                                                  &transfer_ack,
                                                  hdr->retry_count);
                }

                if (transfer_ack == WDAP_ACK_NONE) {
                    transfer_ack = WDAP_ACK_FAULT;
                }
                ack = transfer_ack;
                if (err != ESP_OK || ack != WDAP_ACK_OK) {
                    result_flags = wdap_ack_to_dap_flags(ack);
                    goto done;
                }
                if (!use_match || ((value & match_mask) == match_value)) {
                    break;
                }
                if (retries == 0U) {
                    result_flags = DAP_TRANSFER_MISMATCH;
                    goto done;
                }
                --retries;
            } while (true);

            if ((size_t)(&resp_payload[WDAP_MAX_PAYLOAD] - dst) < sizeof(uint32_t)) {
                return ESP_ERR_INVALID_SIZE;
            }
            write_u32_le(dst, value);
            dst += sizeof(uint32_t);
        } else {
            if ((size_t)(end - cursor) < sizeof(uint32_t)) {
                return ESP_ERR_INVALID_ARG;
            }

            const uint32_t value = read_u32_le(cursor);
            cursor += sizeof(uint32_t);

            if (write_match_mask) {
                match_mask = value;
                check_write = false;
            } else {
                if (apndp) {
                    err = execute_write_with_retry(swd_engine_write_ap,
                                                   addr,
                                                   value,
                                                   &transfer_ack,
                                                   hdr->retry_count);
                } else {
                    err = execute_write_with_retry(swd_engine_write_dp,
                                                   addr,
                                                   value,
                                                   &transfer_ack,
                                                   hdr->retry_count);
                }

                if (transfer_ack == WDAP_ACK_NONE) {
                    transfer_ack = WDAP_ACK_FAULT;
                }
                ack = transfer_ack;
                if (err != ESP_OK || ack != WDAP_ACK_OK) {
                    result_flags = wdap_ack_to_dap_flags(ack);
                    goto done;
                }
                check_write = true;
            }
        }

        ++completed;
    }

    if (result_flags == DAP_TRANSFER_OK && check_write) {
        uint32_t ignored = 0;
        uint8_t transfer_ack = WDAP_ACK_OK;
        const esp_err_t err = swd_engine_read_dp(0x0CU, &ignored, &transfer_ack);
        if (transfer_ack == WDAP_ACK_NONE) {
            transfer_ack = WDAP_ACK_FAULT;
        }
        ack = transfer_ack;
        if (err != ESP_OK || ack != WDAP_ACK_OK) {
            result_flags = wdap_ack_to_dap_flags(ack);
        }
    }

done:
    response->ack = ack;
    wdap_transfer_sequence_response_t *resp_hdr = (wdap_transfer_sequence_response_t *)resp_payload;
    resp_hdr->completed = completed;
    resp_hdr->result_flags = result_flags;
    return append_payload(response, resp_payload, (size_t)(dst - resp_payload));
}

static esp_err_t handle_request(const wdap_message_t *request, wdap_message_t *response)
{
    if (wdap_runtime_is_busy() &&
        request->cmd != WDAP_CMD_PING &&
        request->cmd != WDAP_CMD_GET_VERSION &&
        request->cmd != WDAP_CMD_GET_CAPS) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (request->cmd) {
    case WDAP_CMD_PING:
        return handle_ping(request, response);
    case WDAP_CMD_GET_VERSION:
        return handle_get_version(response);
    case WDAP_CMD_GET_CAPS:
        return handle_get_caps(response);
    case WDAP_CMD_SET_SWD_FREQ:
        return handle_set_swd_freq(request, response);
    case WDAP_CMD_SWD_LINE_RESET:
        return handle_line_reset(response);
    case WDAP_CMD_TARGET_RESET:
        return handle_target_reset(response);
    case WDAP_CMD_TARGET_RESET_DRIVE:
        return handle_target_reset_drive(request, response);
    case WDAP_CMD_SET_TRANSFER_CONFIG:
        return handle_transfer_config(request, response);
    case WDAP_CMD_SWJ_PINS:
        return handle_swj_pins(request, response);
    case WDAP_CMD_SWD_SEQUENCE:
        return handle_swd_sequence(request, response);
    case WDAP_CMD_TARGET_HALT:
        return handle_target_halt(response);
    case WDAP_CMD_READ_DP_IDCODE:
        return handle_read_dp(0x00U, response);
    case WDAP_CMD_SWD_READ_DP:
        if (request->payload_len < sizeof(wdap_reg_read_request_t)) {
            return ESP_ERR_INVALID_ARG;
        }
        return handle_read_dp(((const wdap_reg_read_request_t *)request->payload)->addr, response);
    case WDAP_CMD_SWD_WRITE_DP:
        return handle_write_dp(request, response);
    case WDAP_CMD_SWD_READ_AP:
        if (request->payload_len < sizeof(wdap_reg_read_request_t)) {
            return ESP_ERR_INVALID_ARG;
        }
        return handle_read_ap(((const wdap_reg_read_request_t *)request->payload)->addr, response);
    case WDAP_CMD_SWD_WRITE_AP:
        return handle_write_ap(request, response);
    case WDAP_CMD_SWD_READ_BLOCK:
        return handle_read_block(request, response);
    case WDAP_CMD_SWD_WRITE_BLOCK:
        return handle_write_block(request, response);
    case WDAP_CMD_SWD_TRANSFER_SEQUENCE:
        return handle_transfer_sequence(request, response);
    case WDAP_CMD_SWJ_SEQUENCE: {
        if (request->payload_len < 1U) {
            return ESP_ERR_INVALID_ARG;
        }
        uint32_t count = request->payload[0];
        if (count == 0U) {
            count = 256U;
        }
        const uint32_t byte_count = (count + 7U) / 8U;
        if (request->payload_len < 1U + byte_count) {
            return ESP_ERR_INVALID_ARG;
        }
        response->ack = WDAP_ACK_OK;
        return swd_engine_swj_sequence(count, &request->payload[1]);
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t dap_backend_process_frame(const uint8_t *rx_data,
                                    size_t rx_len,
                                    uint8_t *tx_data,
                                    size_t tx_capacity,
                                    size_t *tx_len)
{
    if (rx_data == NULL || tx_data == NULL || tx_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wdap_message_t request;
    ESP_RETURN_ON_ERROR(transport_proto_decode(rx_data, rx_len, &request), TAG, "decode request failed");

    if (request.msg_type != WDAP_MSG_REQUEST && request.msg_type != WDAP_MSG_HEARTBEAT) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    wdap_message_t response = {
        .msg_type = WDAP_MSG_RESPONSE,
        .cmd = request.cmd,
        .status = WDAP_STATUS_OK,
        .ack = WDAP_ACK_NONE,
        .seq = request.seq,
        .payload_len = 0,
    };

    const esp_err_t handler_err = handle_request(&request, &response);
    response.status = status_from_err(handler_err);

    if (request.cmd == WDAP_CMD_SWD_TRANSFER_SEQUENCE ||
        request.cmd == WDAP_CMD_SWD_READ_BLOCK ||
        request.cmd == WDAP_CMD_SWD_WRITE_BLOCK ||
        request.cmd == WDAP_CMD_SWD_READ_DP ||
        request.cmd == WDAP_CMD_SWD_WRITE_DP ||
        request.cmd == WDAP_CMD_SWD_READ_AP ||
        request.cmd == WDAP_CMD_SWD_WRITE_AP) {
        ESP_LOGD(TAG, "cmd=%s seq=%u status=%s ack=0x%02x",
                 wdap_cmd_to_string(request.cmd),
                 request.seq,
                 wdap_status_to_string(response.status),
                 response.ack);
    } else {
        ESP_LOGI(TAG, "cmd=%s seq=%u status=%s ack=0x%02x",
                 wdap_cmd_to_string(request.cmd),
                 request.seq,
                 wdap_status_to_string(response.status),
                 response.ack);
    }

    return transport_proto_encode(&response, tx_data, tx_capacity, tx_len);
}
