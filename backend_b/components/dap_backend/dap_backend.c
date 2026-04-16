#include "dap_backend.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "log_utils.h"
#include "swd_engine.h"
#include "transport_proto.h"

static const char *TAG = "dap_backend";

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
        .feature_flags = swd_engine_get_capabilities(),
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
                              ? swd_engine_read_dp_idcode(&payload.value, &ack)
                              : swd_engine_read_dp(addr, &payload.value, &ack);
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
    const esp_err_t err = swd_engine_write_dp(write->addr, write->value, &ack);
    response->ack = ack;
    return err;
}

static esp_err_t handle_read_ap(uint8_t addr, wdap_message_t *response)
{
    uint8_t ack = WDAP_ACK_NONE;
    wdap_reg_value_response_t payload = {0};
    const esp_err_t err = swd_engine_read_ap(addr, &payload.value, &ack);
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
    const esp_err_t err = swd_engine_write_ap(write->addr, write->value, &ack);
    response->ack = ack;
    return err;
}

static esp_err_t handle_request(const wdap_message_t *request, wdap_message_t *response)
{
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
    case WDAP_CMD_SWD_WRITE_BLOCK:
        return ESP_ERR_NOT_SUPPORTED;
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

    ESP_LOGI(TAG, "cmd=%s seq=%u status=%s ack=0x%02x",
             wdap_cmd_to_string(request.cmd),
             request.seq,
             wdap_status_to_string(response.status),
             response.ack);

    return transport_proto_encode(&response, tx_data, tx_capacity, tx_len);
}
