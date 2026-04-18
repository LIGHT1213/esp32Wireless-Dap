#include "uart_bridge.h"

#include <inttypes.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "transport_proto.h"
#include "wdap_runtime.h"

static const char *TAG = "uart_bridge";

esp_err_t wifi_link_send_packet(const uint8_t *data, size_t len);

#define UART_BRIDGE_RX_BUFFER_SIZE 2048U
#define UART_BRIDGE_TX_BUFFER_SIZE 1024U
#define UART_BRIDGE_MAX_CHUNK 256U
#define UART_BRIDGE_TASK_STACK_SIZE 4096U
#define UART_BRIDGE_TASK_PRIORITY 4U
#define UART_BRIDGE_CORE_ID 0

typedef struct {
    uart_port_t port_num;
    wdap_uart_config_t current_config;
    TaskHandle_t rx_task;
    uint16_t next_seq;
    bool initialized;
} uart_bridge_state_t;

static uart_bridge_state_t s_state = {
    .port_num = (uart_port_t)CONFIG_WDAP_UART_BRIDGE_PORT_NUM,
    .current_config = {
        .bit_rate = CONFIG_WDAP_UART_BRIDGE_BAUDRATE,
        .stop_bits = 0,
        .parity = 0,
        .data_bits = 8,
        .dtr = 0,
        .rts = 0,
        .reserved = {0, 0},
    },
    .next_seq = 1,
};

static esp_err_t map_stop_bits(uint8_t stop_bits, uart_stop_bits_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (stop_bits) {
    case 0:
        *out = UART_STOP_BITS_1;
        return ESP_OK;
    case 1:
        *out = UART_STOP_BITS_1_5;
        return ESP_OK;
    case 2:
        *out = UART_STOP_BITS_2;
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t map_parity(uint8_t parity, uart_parity_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (parity) {
    case 0:
        *out = UART_PARITY_DISABLE;
        return ESP_OK;
    case 1:
        *out = UART_PARITY_ODD;
        return ESP_OK;
    case 2:
        *out = UART_PARITY_EVEN;
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t map_data_bits(uint8_t data_bits, uart_word_length_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (data_bits) {
    case 5:
        *out = UART_DATA_5_BITS;
        return ESP_OK;
    case 6:
        *out = UART_DATA_6_BITS;
        return ESP_OK;
    case 7:
        *out = UART_DATA_7_BITS;
        return ESP_OK;
    case 8:
        *out = UART_DATA_8_BITS;
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t apply_uart_config(const wdap_uart_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uart_word_length_t data_bits = UART_DATA_8_BITS;
    uart_parity_t parity = UART_PARITY_DISABLE;
    uart_stop_bits_t stop_bits = UART_STOP_BITS_1;

    ESP_RETURN_ON_ERROR(map_data_bits(config->data_bits, &data_bits), TAG, "unsupported data bits");
    ESP_RETURN_ON_ERROR(map_parity(config->parity, &parity), TAG, "unsupported parity");
    ESP_RETURN_ON_ERROR(map_stop_bits(config->stop_bits, &stop_bits), TAG, "unsupported stop bits");

    const uart_config_t uart_config = {
        .baud_rate = (int)config->bit_rate,
        .data_bits = data_bits,
        .parity = parity,
        .stop_bits = stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_APB,
    };

    ESP_RETURN_ON_ERROR(uart_param_config(s_state.port_num, &uart_config), TAG, "uart_param_config failed");
    s_state.current_config = *config;
    return ESP_OK;
}

static esp_err_t send_uart_stream(const uint8_t *data, uint16_t len)
{
    uint8_t encoded[WDAP_MAX_FRAME_SIZE];
    size_t encoded_size = 0;
    wdap_message_t message = {
        .msg_type = WDAP_MSG_STREAM,
        .cmd = WDAP_CMD_UART_DATA,
        .status = WDAP_STATUS_OK,
        .ack = WDAP_ACK_NONE,
        .seq = s_state.next_seq++,
        .payload_len = len,
    };

    if (len > 0U && data != NULL) {
        memcpy(message.payload, data, len);
    }

    ESP_RETURN_ON_ERROR(transport_proto_encode(&message, encoded, sizeof(encoded), &encoded_size),
                        TAG,
                        "encode uart stream failed");
    return wifi_link_send_packet(encoded, encoded_size);
}

static void uart_bridge_rx_task(void *arg)
{
    (void)arg;

    uint8_t buffer[UART_BRIDGE_MAX_CHUNK];

    while (true) {
        const int rx_len = uart_read_bytes(s_state.port_num,
                                           buffer,
                                           sizeof(buffer),
                                           pdMS_TO_TICKS(20));
        if (rx_len <= 0) {
            continue;
        }
        if (wdap_runtime_is_busy()) {
            continue;
        }

        if (send_uart_stream(buffer, (uint16_t)rx_len) != ESP_OK) {
            ESP_LOGW(TAG, "drop UART RX chunk len=%d because frontend link is not ready", rx_len);
        }
    }
}

esp_err_t uart_bridge_init(void)
{
    if (s_state.initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(uart_driver_install(s_state.port_num,
                                            UART_BRIDGE_RX_BUFFER_SIZE,
                                            UART_BRIDGE_TX_BUFFER_SIZE,
                                            0,
                                            NULL,
                                            0),
                        TAG,
                        "uart_driver_install failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(s_state.port_num,
                                     CONFIG_WDAP_UART_BRIDGE_TX_GPIO,
                                     CONFIG_WDAP_UART_BRIDGE_RX_GPIO,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG,
                        "uart_set_pin failed");
    ESP_RETURN_ON_ERROR(apply_uart_config(&s_state.current_config), TAG, "apply default uart config failed");

    const BaseType_t ok = xTaskCreatePinnedToCore(uart_bridge_rx_task,
                                                  "uart_bridge_rx",
                                                  UART_BRIDGE_TASK_STACK_SIZE,
                                                  NULL,
                                                  UART_BRIDGE_TASK_PRIORITY,
                                                  &s_state.rx_task,
                                                  UART_BRIDGE_CORE_ID);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    s_state.initialized = true;
    ESP_LOGI(TAG, "uart bridge ready on UART%d tx=%d rx=%d baud=%" PRIu32,
             CONFIG_WDAP_UART_BRIDGE_PORT_NUM,
             CONFIG_WDAP_UART_BRIDGE_TX_GPIO,
             CONFIG_WDAP_UART_BRIDGE_RX_GPIO,
             s_state.current_config.bit_rate);
    return ESP_OK;
}

esp_err_t uart_bridge_handle_message(const wdap_message_t *message)
{
    if (message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (message->cmd) {
    case WDAP_CMD_UART_DATA: {
        if (wdap_runtime_is_busy()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (message->payload_len == 0U) {
            return ESP_OK;
        }
        const int written = uart_write_bytes(s_state.port_num,
                                             (const char *)message->payload,
                                             message->payload_len);
        return (written == (int)message->payload_len) ? ESP_OK : ESP_FAIL;
    }
    case WDAP_CMD_UART_CONFIG:
        if (wdap_runtime_is_busy()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (message->payload_len < sizeof(wdap_uart_config_t)) {
            return ESP_ERR_INVALID_ARG;
        }
        return apply_uart_config((const wdap_uart_config_t *)message->payload);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}
