#include "cmsis_dap_usb.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "class/hid/hid_device.h"
#include "class/vendor/vendor_device.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "session_mgr.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "wdap_protocol.h"

static const char *TAG = "cmsis_dap_usb";

#define CMSIS_DAP_PACKET_SIZE 64U
#define CMSIS_DAP_HID_EP_OUT 0x01
#define CMSIS_DAP_HID_EP_IN 0x81
#define CMSIS_DAP_VENDOR_EP_OUT 0x02
#define CMSIS_DAP_VENDOR_EP_IN 0x82
#define CMSIS_DAP_USB_QUEUE_LEN 4U
#define CMSIS_DAP_VENDOR_REQUEST_MICROSOFT 0x20U
#define CMSIS_DAP_MAX_SWJ_CLOCK_HZ 1000000U

#define ID_DAP_INFO 0x00U
#define ID_DAP_HOST_STATUS 0x01U
#define ID_DAP_CONNECT 0x02U
#define ID_DAP_DISCONNECT 0x03U
#define ID_DAP_TRANSFER_CONFIGURE 0x04U
#define ID_DAP_TRANSFER 0x05U
#define ID_DAP_TRANSFER_BLOCK 0x06U
#define ID_DAP_TRANSFER_ABORT 0x07U
#define ID_DAP_WRITE_ABORT 0x08U
#define ID_DAP_DELAY 0x09U
#define ID_DAP_RESET_TARGET 0x0AU
#define ID_DAP_SWJ_PINS 0x10U
#define ID_DAP_SWJ_CLOCK 0x11U
#define ID_DAP_SWJ_SEQUENCE 0x12U
#define ID_DAP_SWD_CONFIGURE 0x13U

#define DAP_OK 0x00U
#define DAP_ERROR 0xFFU

#define DAP_ID_VENDOR 0x01U
#define DAP_ID_PRODUCT 0x02U
#define DAP_ID_SER_NUM 0x03U
#define DAP_ID_DAP_FW_VER 0x04U
#define DAP_ID_DEVICE_VENDOR 0x05U
#define DAP_ID_DEVICE_NAME 0x06U
#define DAP_ID_BOARD_VENDOR 0x07U
#define DAP_ID_BOARD_NAME 0x08U
#define DAP_ID_PRODUCT_FW_VER 0x09U
#define DAP_ID_CAPABILITIES 0xF0U
#define DAP_ID_PACKET_COUNT 0xFEU
#define DAP_ID_PACKET_SIZE 0xFFU

#define DAP_PORT_DISABLED 0x00U
#define DAP_PORT_SWD 0x01U

#define DAP_SWJ_SWCLK_TCK 0U
#define DAP_SWJ_SWDIO_TMS 1U
#define DAP_SWJ_nRESET 7U

#define DAP_TRANSFER_APNDP BIT(0)
#define DAP_TRANSFER_RNW BIT(1)
#define DAP_TRANSFER_A2 BIT(2)
#define DAP_TRANSFER_A3 BIT(3)
#define DAP_TRANSFER_MATCH_VALUE BIT(4)
#define DAP_TRANSFER_MATCH_MASK BIT(5)

#define DAP_TRANSFER_OK BIT(0)
#define DAP_TRANSFER_WAIT BIT(1)
#define DAP_TRANSFER_FAULT BIT(2)
#define DAP_TRANSFER_ERROR BIT(3)
#define DAP_TRANSFER_MISMATCH BIT(4)

#define CMSIS_DAP_CAP_SWD BIT(0)

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_INOUT_DESC_LEN + CFG_TUD_VENDOR * TUD_VENDOR_DESC_LEN)
#define CMSIS_DAP_BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
#define CMSIS_DAP_MS_OS_20_DESC_LEN 0x00B2U

typedef enum {
    CMSIS_DAP_TRANSPORT_HID = 0,
    CMSIS_DAP_TRANSPORT_VENDOR = 1,
} cmsis_dap_transport_t;

typedef struct {
    uint8_t data[CMSIS_DAP_PACKET_SIZE];
    uint16_t len;
    uint8_t transport;
} cmsis_dap_packet_t;

typedef struct {
    QueueHandle_t rx_queue;
    TaskHandle_t worker_task;
    uint8_t debug_port;
    uint8_t host_status[2];
    uint8_t swj_pins;
    uint8_t swd_turnaround;
    uint8_t swd_data_phase;
    uint8_t idle_cycles;
    uint16_t retry_count;
    uint16_t match_retry;
    uint32_t match_mask;
    uint32_t dp_select;
    uint32_t swj_clock_hz;
    char serial[13];
    bool initialized;
} cmsis_dap_state_t;

static cmsis_dap_state_t s_state = {
    .debug_port = DAP_PORT_DISABLED,
    .swd_turnaround = 1,
    .retry_count = 5,
    .match_retry = 5,
    .match_mask = 0xFFFFFFFFU,
    .dp_select = 0,
    .swj_clock_hz = 100000U,
};

static const uint8_t s_hid_report_descriptor[] = {
    0x06, 0x00, 0xFF,
    0x09, 0x01,
    0xA1, 0x01,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, CMSIS_DAP_PACKET_SIZE,
    0x09, 0x01,
    0x81, 0x02,
    0x95, CMSIS_DAP_PACKET_SIZE,
    0x09, 0x01,
    0x91, 0x02,
    0xC0,
};

static const char *s_string_descriptor[] = {
    (char[]){0x09, 0x04},
    "wireless-dap",
    "Wireless CMSIS-DAP",
    s_state.serial,
    "CMSIS-DAP v1",
    "CMSIS-DAP v2",
};

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4042,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_INOUT_DESCRIPTOR(0, 4, false, sizeof(s_hid_report_descriptor), CMSIS_DAP_HID_EP_OUT, CMSIS_DAP_HID_EP_IN, CMSIS_DAP_PACKET_SIZE, 1),
    TUD_VENDOR_DESCRIPTOR(1, 5, CMSIS_DAP_VENDOR_EP_OUT, CMSIS_DAP_VENDOR_EP_IN, CMSIS_DAP_PACKET_SIZE),
};

static const uint8_t s_bos_descriptor[] = {
    TUD_BOS_DESCRIPTOR(CMSIS_DAP_BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(CMSIS_DAP_MS_OS_20_DESC_LEN, CMSIS_DAP_VENDOR_REQUEST_MICROSOFT),
};

static const uint8_t s_ms_os_20_descriptor[] = {
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(CMSIS_DAP_MS_OS_20_DESC_LEN),
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0, U16_TO_U8S_LE(CMSIS_DAP_MS_OS_20_DESC_LEN - 0x000A),
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), 1, 0, U16_TO_U8S_LE(CMSIS_DAP_MS_OS_20_DESC_LEN - 0x000A - 0x0008),
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(CMSIS_DAP_MS_OS_20_DESC_LEN - 0x000A - 0x0008 - 0x0008 - 0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,
    'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050),
    '{', 0x00, 'A', 0x00, '8', 0x00, '8', 0x00, '3', 0x00, '1', 0x00, '2', 0x00, '3', 0x00, '9', 0x00, '-', 0x00,
    'D', 0x00, '7', 0x00, 'C', 0x00, 'B', 0x00, '-', 0x00, '4', 0x00, 'E', 0x00, '9', 0x00, 'C', 0x00, '-', 0x00,
    'A', 0x00, '7', 0x00, '5', 0x00, 'F', 0x00, '-', 0x00, 'E', 0x00, '2', 0x00, 'D', 0x00, 'D', 0x00, 'D', 0x00,
    '3', 0x00, '9', 0x00, '2', 0x00, 'E', 0x00, 'B', 0x00, '3', 0x00, '6', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,
};

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

static uint8_t wdap_ack_to_dap(uint8_t ack, esp_err_t err, uint8_t status)
{
    if (status == WDAP_STATUS_OK) {
        if ((ack & WDAP_ACK_WAIT) != 0U) {
            return DAP_TRANSFER_WAIT;
        }
        if ((ack & WDAP_ACK_FAULT) != 0U) {
            return DAP_TRANSFER_FAULT;
        }
        return DAP_TRANSFER_OK;
    }

    if ((ack & WDAP_ACK_WAIT) != 0U || err == ESP_ERR_TIMEOUT || status == WDAP_STATUS_TIMEOUT) {
        return DAP_TRANSFER_WAIT;
    }
    if ((ack & WDAP_ACK_FAULT) != 0U) {
        return DAP_TRANSFER_FAULT;
    }
    return DAP_TRANSFER_ERROR;
}

static esp_err_t transact(uint8_t cmd, const void *payload, uint16_t payload_len, wdap_message_t *response)
{
    return session_mgr_send_command(cmd, payload, payload_len, response, CONFIG_WDAP_REQUEST_TIMEOUT_MS);
}

static uint8_t do_line_reset(void)
{
    wdap_message_t response = {0};
    if (transact(WDAP_CMD_SWD_LINE_RESET, NULL, 0, &response) != ESP_OK) {
        return DAP_ERROR;
    }
    return (response.status == WDAP_STATUS_OK) ? DAP_OK : DAP_ERROR;
}

static uint8_t do_target_reset(void)
{
    wdap_message_t response = {0};
    if (transact(WDAP_CMD_TARGET_RESET, NULL, 0, &response) != ESP_OK) {
        return DAP_ERROR;
    }
    return (response.status == WDAP_STATUS_OK) ? DAP_OK : DAP_ERROR;
}

static uint8_t do_target_reset_drive(bool asserted)
{
    wdap_target_reset_drive_request_t request = {
        .asserted = asserted ? 1U : 0U,
    };
    wdap_message_t response = {0};
    if (transact(WDAP_CMD_TARGET_RESET_DRIVE, &request, sizeof(request), &response) != ESP_OK) {
        return DAP_ERROR;
    }
    return (response.status == WDAP_STATUS_OK) ? DAP_OK : DAP_ERROR;
}

static uint8_t do_set_clock(uint32_t hz)
{
    const uint32_t applied_hz = (hz > CMSIS_DAP_MAX_SWJ_CLOCK_HZ) ? CMSIS_DAP_MAX_SWJ_CLOCK_HZ : hz;
    wdap_set_swd_freq_request_t request = {
        .hz = applied_hz,
    };
    wdap_message_t response = {0};
    if (transact(WDAP_CMD_SET_SWD_FREQ, &request, sizeof(request), &response) != ESP_OK) {
        return DAP_ERROR;
    }
    if (response.status != WDAP_STATUS_OK) {
        return DAP_ERROR;
    }
    s_state.swj_clock_hz = applied_hz;
    return DAP_OK;
}

static esp_err_t do_read_reg(bool apndp, uint8_t addr, uint32_t *value, uint8_t *response_value)
{
    wdap_message_t response = {0};
    esp_err_t err;
    if (apndp) {
        wdap_reg_read_request_t request = {
            .addr = (uint8_t)((s_state.dp_select & 0xF0U) | (addr & 0x0CU)),
        };
        err = transact(WDAP_CMD_SWD_READ_AP, &request, sizeof(request), &response);
    } else if (addr == 0x00U) {
        err = transact(WDAP_CMD_READ_DP_IDCODE, NULL, 0, &response);
    } else {
        wdap_reg_read_request_t request = {
            .addr = addr,
        };
        err = transact(WDAP_CMD_SWD_READ_DP, &request, sizeof(request), &response);
    }

    *response_value = wdap_ack_to_dap(response.ack, err, response.status);
    if (err != ESP_OK || response.status != WDAP_STATUS_OK || response.payload_len < sizeof(wdap_reg_value_response_t)) {
        ESP_LOGW(TAG, "read %s addr=0x%02x failed err=%s status=%u ack=0x%02x",
                 apndp ? "AP" : "DP",
                 apndp ? (uint8_t)((s_state.dp_select & 0xF0U) | (addr & 0x0CU)) : addr,
                 esp_err_to_name(err),
                 response.status,
                 response.ack);
        return ESP_FAIL;
    }

    *value = ((const wdap_reg_value_response_t *)response.payload)->value;
    return ESP_OK;
}

static esp_err_t do_write_reg(bool apndp, uint8_t addr, uint32_t value, uint8_t *response_value)
{
    wdap_message_t response = {0};
    wdap_reg_write_request_t request = {
        .addr = apndp ? (uint8_t)((s_state.dp_select & 0xF0U) | (addr & 0x0CU)) : addr,
        .value = value,
    };
    const esp_err_t err = transact(apndp ? WDAP_CMD_SWD_WRITE_AP : WDAP_CMD_SWD_WRITE_DP,
                                   &request,
                                   sizeof(request),
                                   &response);
    *response_value = wdap_ack_to_dap(response.ack, err, response.status);
    if (!apndp && addr == 0x08U && err == ESP_OK && response.status == WDAP_STATUS_OK) {
        s_state.dp_select = value;
    }
    if (err != ESP_OK || response.status != WDAP_STATUS_OK) {
        ESP_LOGW(TAG, "write %s addr=0x%02x value=0x%08" PRIx32 " failed err=%s status=%u ack=0x%02x",
                 apndp ? "AP" : "DP",
                 request.addr,
                 value,
                 esp_err_to_name(err),
                 response.status,
                 response.ack);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static size_t handle_dap_info(const uint8_t *request, uint8_t *response)
{
    const char *text = NULL;
    uint8_t length = 0;

    switch (request[1]) {
    case DAP_ID_VENDOR:
        text = "wireless-dap";
        break;
    case DAP_ID_PRODUCT:
        text = "Wireless CMSIS-DAP";
        break;
    case DAP_ID_SER_NUM:
        text = s_state.serial;
        break;
    case DAP_ID_DAP_FW_VER:
        text = "0.1";
        break;
    case DAP_ID_DEVICE_VENDOR:
        text = "STMicroelectronics";
        break;
    case DAP_ID_DEVICE_NAME:
        text = "STM32";
        break;
    case DAP_ID_BOARD_VENDOR:
        text = "wireless-dap";
        break;
    case DAP_ID_BOARD_NAME:
        text = "ESP32-S3 Frontend A";
        break;
    case DAP_ID_PRODUCT_FW_VER:
        text = "0.1";
        break;
    case DAP_ID_CAPABILITIES:
        response[1] = 1;
        response[2] = CMSIS_DAP_CAP_SWD;
        return 3;
    case DAP_ID_PACKET_COUNT:
        response[1] = 1;
        response[2] = 1;
        return 3;
    case DAP_ID_PACKET_SIZE:
        response[1] = 2;
        response[2] = (uint8_t)(CMSIS_DAP_PACKET_SIZE >> 0);
        response[3] = (uint8_t)(CMSIS_DAP_PACKET_SIZE >> 8);
        return 4;
    default:
        response[1] = 0;
        return 2;
    }

    length = (uint8_t)strlen(text);
    response[1] = length;
    memcpy(&response[2], text, length);
    return (size_t)length + 2U;
}

static size_t handle_dap_connect(const uint8_t *request, uint8_t *response)
{
    const uint8_t port = (request[1] == 0U) ? DAP_PORT_SWD : request[1];
    if (port == DAP_PORT_SWD) {
        s_state.debug_port = DAP_PORT_SWD;
        s_state.dp_select = 0;
        (void)do_line_reset();
        response[1] = DAP_PORT_SWD;
    } else {
        s_state.debug_port = DAP_PORT_DISABLED;
        s_state.dp_select = 0;
        response[1] = DAP_PORT_DISABLED;
    }
    return 2;
}

static size_t handle_dap_disconnect(uint8_t *response)
{
    s_state.debug_port = DAP_PORT_DISABLED;
    s_state.dp_select = 0;
    response[1] = DAP_OK;
    return 2;
}

static size_t handle_dap_host_status(const uint8_t *request, uint8_t *response)
{
    s_state.host_status[0] = request[1];
    s_state.host_status[1] = request[2];
    response[1] = DAP_OK;
    return 2;
}

static size_t handle_dap_swj_clock(const uint8_t *request, uint8_t *response)
{
    response[1] = do_set_clock(read_u32_le(&request[1]));
    return 2;
}

static size_t handle_dap_swj_sequence(const uint8_t *request, uint8_t *response)
{
    uint32_t count = request[1];
    if (count == 0U) {
        count = 256U;
    }

    response[1] = DAP_OK;
    if (s_state.debug_port == DAP_PORT_SWD && count >= 16U) {
        response[1] = do_line_reset();
    }
    return 2;
}

static size_t handle_dap_swd_configure(const uint8_t *request, uint8_t *response)
{
    s_state.swd_turnaround = (request[1] & 0x03U) + 1U;
    s_state.swd_data_phase = (request[1] & 0x04U) ? 1U : 0U;
    response[1] = DAP_OK;
    return 2;
}

static size_t handle_dap_transfer_configure(const uint8_t *request, uint8_t *response)
{
    s_state.idle_cycles = request[1];
    s_state.retry_count = (uint16_t)request[2] | ((uint16_t)request[3] << 8);
    s_state.match_retry = (uint16_t)request[4] | ((uint16_t)request[5] << 8);
    response[1] = DAP_OK;
    return 2;
}

static size_t handle_dap_write_abort(const uint8_t *request, uint8_t *response)
{
    uint8_t transfer_value = DAP_TRANSFER_ERROR;
    response[1] = (do_write_reg(false, 0x00U, read_u32_le(&request[2]), &transfer_value) == ESP_OK) ? DAP_OK : DAP_ERROR;
    return 2;
}

static size_t handle_dap_reset_target(uint8_t *response)
{
    response[1] = do_target_reset();
    response[2] = (response[1] == DAP_OK) ? 1U : 0U;
    return 3;
}

static size_t handle_dap_delay(const uint8_t *request, uint8_t *response)
{
    vTaskDelay(pdMS_TO_TICKS((TickType_t)((uint16_t)request[1] | ((uint16_t)request[2] << 8))));
    response[1] = DAP_OK;
    return 2;
}

static size_t handle_dap_swj_pins(const uint8_t *request, uint8_t *response)
{
    const uint8_t value = request[1];
    const uint8_t select = request[2];

    if ((select & BIT(DAP_SWJ_SWCLK_TCK)) != 0U) {
        if ((value & BIT(DAP_SWJ_SWCLK_TCK)) != 0U) {
            s_state.swj_pins |= BIT(DAP_SWJ_SWCLK_TCK);
        } else {
            s_state.swj_pins &= (uint8_t)~BIT(DAP_SWJ_SWCLK_TCK);
        }
    }
    if ((select & BIT(DAP_SWJ_SWDIO_TMS)) != 0U) {
        if ((value & BIT(DAP_SWJ_SWDIO_TMS)) != 0U) {
            s_state.swj_pins |= BIT(DAP_SWJ_SWDIO_TMS);
        } else {
            s_state.swj_pins &= (uint8_t)~BIT(DAP_SWJ_SWDIO_TMS);
        }
    }
    if ((select & BIT(DAP_SWJ_nRESET)) != 0U) {
        const bool deasserted = (value & BIT(DAP_SWJ_nRESET)) != 0U;
        if (do_target_reset_drive(!deasserted) == DAP_OK) {
            if (deasserted) {
                s_state.swj_pins |= BIT(DAP_SWJ_nRESET);
            } else {
                s_state.swj_pins &= (uint8_t)~BIT(DAP_SWJ_nRESET);
            }
        }
    } else {
        s_state.swj_pins |= BIT(DAP_SWJ_nRESET);
    }
    response[1] = s_state.swj_pins;
    return 2;
}

static size_t handle_dap_transfer(const uint8_t *request, uint8_t *response)
{
    const uint8_t *cursor = &request[3];
    const uint8_t request_count = request[2];
    uint8_t *payload = &response[3];
    uint8_t completed = 0;
    uint8_t response_value = 0;

    if (s_state.debug_port != DAP_PORT_SWD) {
        response[1] = 0;
        response[2] = 0;
        return 3;
    }

    for (uint8_t i = 0; i < request_count; ++i) {
        const uint8_t request_value = *cursor++;
        const bool apndp = (request_value & DAP_TRANSFER_APNDP) != 0U;
        const bool read = (request_value & DAP_TRANSFER_RNW) != 0U;
        const uint8_t addr = request_value & 0x0CU;
        uint32_t value = 0;
        esp_err_t err = ESP_OK;

        if (read) {
            uint32_t match_value = 0;
            bool use_match = false;
            if ((request_value & DAP_TRANSFER_MATCH_VALUE) != 0U) {
                use_match = true;
                match_value = read_u32_le(cursor);
                cursor += 4;
            }

            uint16_t retries = s_state.match_retry;
            do {
                err = do_read_reg(apndp, addr, &value, &response_value);
                if (err != ESP_OK || !use_match) {
                    break;
                }
                if ((value & s_state.match_mask) == match_value) {
                    break;
                }
                if (retries == 0U) {
                    response_value = DAP_TRANSFER_MISMATCH;
                    break;
                }
                --retries;
            } while (true);

            if (response_value != DAP_TRANSFER_OK) {
                break;
            }
            write_u32_le(payload, value);
            payload += sizeof(uint32_t);
        } else {
            value = read_u32_le(cursor);
            cursor += 4;

            if ((request_value & DAP_TRANSFER_MATCH_MASK) != 0U) {
                s_state.match_mask = value;
                response_value = DAP_TRANSFER_OK;
            } else {
                err = do_write_reg(apndp, addr, value, &response_value);
                if (err != ESP_OK || response_value != DAP_TRANSFER_OK) {
                    break;
                }
            }
        }

        ++completed;
    }

    response[1] = completed;
    response[2] = response_value;
    return (size_t)(payload - response);
}

static size_t handle_dap_transfer_block(const uint8_t *request, uint8_t *response)
{
    const uint8_t request_value = request[4];
    const bool apndp = (request_value & DAP_TRANSFER_APNDP) != 0U;
    const bool read = (request_value & DAP_TRANSFER_RNW) != 0U;
    const uint8_t addr = request_value & 0x0CU;
    const uint16_t request_count = (uint16_t)request[2] | ((uint16_t)request[3] << 8);
    const uint8_t *cursor = &request[5];
    uint8_t *payload = &response[4];
    uint16_t completed = 0;
    uint8_t response_value = 0;

    if (s_state.debug_port != DAP_PORT_SWD) {
        response[1] = 0;
        response[2] = 0;
        response[3] = 0;
        return 4;
    }

    for (uint16_t i = 0; i < request_count; ++i) {
        uint32_t value = 0;
        esp_err_t err = ESP_OK;

        if (read) {
            err = do_read_reg(apndp, addr, &value, &response_value);
            if (err != ESP_OK || response_value != DAP_TRANSFER_OK) {
                break;
            }
            write_u32_le(payload, value);
            payload += sizeof(uint32_t);
        } else {
            value = read_u32_le(cursor);
            cursor += sizeof(uint32_t);
            err = do_write_reg(apndp, addr, value, &response_value);
            if (err != ESP_OK || response_value != DAP_TRANSFER_OK) {
                break;
            }
        }
        ++completed;
    }

    response[1] = (uint8_t)(completed >> 0);
    response[2] = (uint8_t)(completed >> 8);
    response[3] = response_value;
    return (size_t)(payload - response);
}

static size_t process_request(const cmsis_dap_packet_t *request, uint8_t *response)
{
    memset(response, 0, CMSIS_DAP_PACKET_SIZE);
    response[0] = request->data[0];
    if (request->transport == CMSIS_DAP_TRANSPORT_HID) {
        ESP_LOGI(TAG, "hid process cmd=0x%02x len=%u", request->data[0], request->len);
    }

    switch (request->data[0]) {
    case ID_DAP_INFO:
        return handle_dap_info(request->data, response);
    case ID_DAP_HOST_STATUS:
        return handle_dap_host_status(request->data, response);
    case ID_DAP_CONNECT:
        return handle_dap_connect(request->data, response);
    case ID_DAP_DISCONNECT:
        return handle_dap_disconnect(response);
    case ID_DAP_SWJ_CLOCK:
        return handle_dap_swj_clock(request->data, response);
    case ID_DAP_SWJ_SEQUENCE:
        return handle_dap_swj_sequence(request->data, response);
    case ID_DAP_SWD_CONFIGURE:
        return handle_dap_swd_configure(request->data, response);
    case ID_DAP_TRANSFER_CONFIGURE:
        return handle_dap_transfer_configure(request->data, response);
    case ID_DAP_TRANSFER:
        return handle_dap_transfer(request->data, response);
    case ID_DAP_TRANSFER_BLOCK:
        return handle_dap_transfer_block(request->data, response);
    case ID_DAP_WRITE_ABORT:
        return handle_dap_write_abort(request->data, response);
    case ID_DAP_RESET_TARGET:
        return handle_dap_reset_target(response);
    case ID_DAP_DELAY:
        return handle_dap_delay(request->data, response);
    case ID_DAP_SWJ_PINS:
        return handle_dap_swj_pins(request->data, response);
    case ID_DAP_TRANSFER_ABORT:
        response[1] = DAP_OK;
        return 2;
    default:
        response[0] = 0xFFU;
        return 1;
    }
}

static void cmsis_dap_worker_task(void *arg)
{
    (void)arg;

    cmsis_dap_packet_t packet;
    uint8_t response[CMSIS_DAP_PACKET_SIZE];

    while (true) {
        if (xQueueReceive(s_state.rx_queue, &packet, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const size_t response_len = process_request(&packet, response);
        const uint16_t hid_send_len = (uint16_t)((response_len < CMSIS_DAP_PACKET_SIZE) ? CMSIS_DAP_PACKET_SIZE : response_len);
        const uint16_t bulk_send_len = (uint16_t)response_len;

        if (packet.transport == CMSIS_DAP_TRANSPORT_VENDOR) {
            while (!tud_mounted() || !tud_vendor_n_mounted(0) || tud_vendor_n_write_available(0) < bulk_send_len) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            if (tud_vendor_n_write(0, response, bulk_send_len) != bulk_send_len) {
                ESP_LOGW(TAG, "failed to queue vendor response cmd=0x%02x", response[0]);
                continue;
            }
            uint32_t flushed = 0;
            for (int retry = 0; retry < 100 && flushed == 0U; ++retry) {
                flushed = tud_vendor_n_write_flush(0);
                if (flushed == 0U) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
            }
            if (flushed == 0U) {
                ESP_LOGW(TAG, "failed to flush vendor response cmd=0x%02x", response[0]);
            }
            continue;
        }

        while (!tud_mounted() || !tud_hid_ready()) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (!tud_hid_report(0, response, hid_send_len)) {
            ESP_LOGW(TAG, "failed to send HID response cmd=0x%02x", response[0]);
        }
    }
}

uint8_t const *tud_descriptor_bos_cb(void)
{
    return s_bos_descriptor;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer,
                           uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;

    cmsis_dap_packet_t packet = {0};
    packet.len = bufsize;
    packet.transport = CMSIS_DAP_TRANSPORT_HID;
    memcpy(packet.data, buffer, bufsize > CMSIS_DAP_PACKET_SIZE ? CMSIS_DAP_PACKET_SIZE : bufsize);
    ESP_LOGI(TAG, "hid rx report_id=%u type=%u len=%u cmd=0x%02x b0=0x%02x b1=0x%02x",
             report_id, report_type, packet.len, packet.data[0], packet.data[0], packet.data[1]);

    if (s_state.rx_queue == NULL || xQueueSend(s_state.rx_queue, &packet, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop HID request because queue is full");
    }
}

#if (TUSB_VERSION_MINOR >= 17)
void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    (void)itf;

    cmsis_dap_packet_t packet = {0};
    packet.len = bufsize;
    packet.transport = CMSIS_DAP_TRANSPORT_VENDOR;
    memcpy(packet.data, buffer, bufsize > CMSIS_DAP_PACKET_SIZE ? CMSIS_DAP_PACKET_SIZE : bufsize);

    if (s_state.rx_queue == NULL || xQueueSend(s_state.rx_queue, &packet, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop vendor request because queue is full");
    }
    tud_vendor_n_read_flush(0);
}
#else
void tud_vendor_rx_cb(uint8_t itf)
{
    (void)itf;

    cmsis_dap_packet_t packet = {0};
    packet.transport = CMSIS_DAP_TRANSPORT_VENDOR;
    packet.len = (uint16_t)tud_vendor_n_read(0, packet.data, sizeof(packet.data));
    if (packet.len == 0U) {
        return;
    }

    if (s_state.rx_queue == NULL || xQueueSend(s_state.rx_queue, &packet, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop vendor request because queue is full");
    }
}
#endif

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == CMSIS_DAP_VENDOR_REQUEST_MICROSOFT &&
        request->wIndex == 7U) {
        uint16_t total_len = 0;
        memcpy(&total_len, s_ms_os_20_descriptor + 8, sizeof(total_len));
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)s_ms_os_20_descriptor, total_len);
    }

    return false;
}

esp_err_t cmsis_dap_usb_init(void)
{
    if (s_state.initialized) {
        return ESP_OK;
    }

    uint8_t mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "read mac failed");
    snprintf(s_state.serial, sizeof(s_state.serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    s_state.rx_queue = xQueueCreate(CMSIS_DAP_USB_QUEUE_LEN, sizeof(cmsis_dap_packet_t));
    if (s_state.rx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &s_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = s_configuration_descriptor;
    tusb_cfg.descriptor.string = s_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(s_string_descriptor) / sizeof(s_string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_configuration_descriptor;
#endif
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb install failed");

    const BaseType_t ok = xTaskCreate(cmsis_dap_worker_task,
                                      "cmsis_dap_usb",
                                      6144,
                                      NULL,
                                      5,
                                      &s_state.worker_task);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    s_state.initialized = true;
    ESP_LOGI(TAG, "USB CMSIS-DAP composite ready on native USB");
    return ESP_OK;
}
