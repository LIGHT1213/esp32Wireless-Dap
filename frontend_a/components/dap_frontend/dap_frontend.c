#include "dap_frontend.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "log_utils.h"
#include "sdkconfig.h"
#include "session_mgr.h"

static const char *TAG = "dap_frontend";

static uint32_t read_u32_le(const uint8_t *buffer)
{
    return (uint32_t)buffer[0] |
           ((uint32_t)buffer[1] << 8) |
           ((uint32_t)buffer[2] << 16) |
           ((uint32_t)buffer[3] << 24);
}

static void print_status_line(const wdap_message_t *response)
{
    printf("status=%s ack=0x%02x cmd=%s seq=%u\n",
           wdap_status_to_string(response->status),
           response->ack,
           wdap_cmd_to_string(response->cmd),
           response->seq);
}

static void print_help(void)
{
    printf("help\n");
    printf("ping\n");
    printf("version\n");
    printf("caps\n");
    printf("line_reset\n");
    printf("target_reset\n");
    printf("halt\n");
    printf("read_dp_idcode\n");
    printf("read_dp <addr>\n");
    printf("read_ap <addr>\n");
    printf("write_ap <addr> <value>\n");
    printf("set_freq <hz>\n");
}

static esp_err_t transact(uint8_t cmd, const void *payload, uint16_t payload_len, wdap_message_t *response)
{
    const esp_err_t err = session_mgr_send_command(cmd, payload, payload_len, response, CONFIG_WDAP_REQUEST_TIMEOUT_MS);
    if (err != ESP_OK) {
        printf("transport error: %s\n", esp_err_to_name(err));
        return err;
    }

    print_status_line(response);
    return ESP_OK;
}

esp_err_t dap_frontend_init(void)
{
    ESP_LOGI(TAG, "dap frontend ready");
    return ESP_OK;
}

void dap_frontend_handle_host_line(const char *line, void *ctx)
{
    (void)ctx;

    char command[128];
    strlcpy(command, line, sizeof(command));

    char *saveptr = NULL;
    const char *token = strtok_r(command, " ", &saveptr);
    if (token == NULL) {
        return;
    }

    if (strcmp(token, "help") == 0) {
        print_help();
        return;
    }

    wdap_message_t response = {0};

    if (strcmp(token, "ping") == 0) {
        wdap_ping_request_t request = {
            .nonce = log_utils_uptime_ms(),
        };
        if (transact(WDAP_CMD_PING, &request, sizeof(request), &response) == ESP_OK &&
            response.status == WDAP_STATUS_OK &&
            response.payload_len >= sizeof(wdap_ping_response_t)) {
            const wdap_ping_response_t *ping = (const wdap_ping_response_t *)response.payload;
            printf("pong nonce=%" PRIu32 " backend_uptime_ms=%" PRIu32 "\n", ping->nonce, ping->uptime_ms);
        }
        return;
    }

    if (strcmp(token, "version") == 0) {
        if (transact(WDAP_CMD_GET_VERSION, NULL, 0, &response) == ESP_OK &&
            response.status == WDAP_STATUS_OK) {
            printf("backend_version=%.*s\n", response.payload_len, (const char *)response.payload);
        }
        return;
    }

    if (strcmp(token, "caps") == 0) {
        if (transact(WDAP_CMD_GET_CAPS, NULL, 0, &response) == ESP_OK &&
            response.status == WDAP_STATUS_OK &&
            response.payload_len >= sizeof(wdap_caps_response_t)) {
            const wdap_caps_response_t *caps = (const wdap_caps_response_t *)response.payload;
            printf("features=0x%08" PRIx32 " max_payload=%" PRIu32 " default_hz=%" PRIu32 " current_hz=%" PRIu32 "\n",
                   caps->feature_flags,
                   caps->max_payload,
                   caps->default_swd_hz,
                   caps->current_swd_hz);
        }
        return;
    }

    if (strcmp(token, "line_reset") == 0) {
        (void)transact(WDAP_CMD_SWD_LINE_RESET, NULL, 0, &response);
        return;
    }

    if (strcmp(token, "target_reset") == 0) {
        (void)transact(WDAP_CMD_TARGET_RESET, NULL, 0, &response);
        return;
    }

    if (strcmp(token, "halt") == 0) {
        if (transact(WDAP_CMD_TARGET_HALT, NULL, 0, &response) == ESP_OK &&
            response.status == WDAP_STATUS_OK &&
            response.payload_len >= sizeof(wdap_reg_value_response_t)) {
            const wdap_reg_value_response_t *value = (const wdap_reg_value_response_t *)response.payload;
            printf("dhcsr=0x%08" PRIx32 "\n", value->value);
        }
        return;
    }

    if (strcmp(token, "read_dp_idcode") == 0) {
        if (transact(WDAP_CMD_READ_DP_IDCODE, NULL, 0, &response) == ESP_OK &&
            response.status == WDAP_STATUS_OK &&
            response.payload_len >= sizeof(wdap_reg_value_response_t)) {
            const wdap_reg_value_response_t *value = (const wdap_reg_value_response_t *)response.payload;
            printf("dp_idcode=0x%08" PRIx32 "\n", value->value);
        }
        return;
    }

    if (strcmp(token, "read_dp") == 0) {
        const char *addr_text = strtok_r(NULL, " ", &saveptr);
        if (addr_text == NULL) {
            printf("usage: read_dp <addr>\n");
            return;
        }

        wdap_reg_read_request_t request = {
            .addr = (uint8_t)strtoul(addr_text, NULL, 0),
        };
        if (transact(WDAP_CMD_SWD_READ_DP, &request, sizeof(request), &response) == ESP_OK &&
            response.status == WDAP_STATUS_OK &&
            response.payload_len >= sizeof(wdap_reg_value_response_t)) {
            const wdap_reg_value_response_t *value = (const wdap_reg_value_response_t *)response.payload;
            printf("dp[0x%02x]=0x%08" PRIx32 "\n", request.addr, value->value);
        }
        return;
    }

    if (strcmp(token, "read_ap") == 0) {
        const char *addr_text = strtok_r(NULL, " ", &saveptr);
        if (addr_text == NULL) {
            printf("usage: read_ap <addr>\n");
            return;
        }

        wdap_reg_read_request_t request = {
            .addr = (uint8_t)strtoul(addr_text, NULL, 0),
        };
        if (transact(WDAP_CMD_SWD_READ_AP, &request, sizeof(request), &response) == ESP_OK &&
            response.status == WDAP_STATUS_OK &&
            response.payload_len >= sizeof(wdap_reg_value_response_t)) {
            const wdap_reg_value_response_t *value = (const wdap_reg_value_response_t *)response.payload;
            printf("ap[0x%02x]=0x%08" PRIx32 "\n", request.addr, value->value);
        }
        return;
    }

    if (strcmp(token, "write_ap") == 0) {
        const char *addr_text = strtok_r(NULL, " ", &saveptr);
        const char *value_text = strtok_r(NULL, " ", &saveptr);
        if (addr_text == NULL || value_text == NULL) {
            printf("usage: write_ap <addr> <value>\n");
            return;
        }

        wdap_reg_write_request_t request = {
            .addr = (uint8_t)strtoul(addr_text, NULL, 0),
            .value = (uint32_t)strtoul(value_text, NULL, 0),
        };
        (void)transact(WDAP_CMD_SWD_WRITE_AP, &request, sizeof(request), &response);
        return;
    }

    if (strcmp(token, "set_freq") == 0) {
        const char *hz_text = strtok_r(NULL, " ", &saveptr);
        if (hz_text == NULL) {
            printf("usage: set_freq <hz>\n");
            return;
        }

        wdap_set_swd_freq_request_t request = {
            .hz = (uint32_t)strtoul(hz_text, NULL, 0),
        };
        if (transact(WDAP_CMD_SET_SWD_FREQ, &request, sizeof(request), &response) == ESP_OK &&
            response.status == WDAP_STATUS_OK &&
            response.payload_len >= sizeof(wdap_reg_value_response_t)) {
            printf("swd_hz=%" PRIu32 "\n", read_u32_le(response.payload));
        }
        return;
    }

    printf("unknown command: %s\n", token);
    print_help();
}
