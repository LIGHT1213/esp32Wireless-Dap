#include "usb_dap_frontend.h"

#include <string.h>
#include "board_config.h"
#if CONFIG_WDAP_USB_HID_FALLBACK
#include "class/hid/hid_device.h"
#endif
#include "class/vendor/vendor_device.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "wdap_transport.h"

#ifndef CONFIG_WDAP_USB_HID_FALLBACK
#define CONFIG_WDAP_USB_HID_FALLBACK 0
#endif

#define WDAP_USB_BULK_EP_SIZE   64
#define WDAP_USB_DEBUG_LOG_LIMIT 0U
#define WDAP_USB_VENDOR_V2       1
#define WDAP_USB_ASYNC_WRITE_PIPELINE 1
#define WDAP_USB_ASYNC_NO_RESPONSE    1
#define WDAP_USB_ASYNC_BATCH_NORESP   1
#define WDAP_USB_ASYNC_BATCH_MAX_CMDS 64U
#define WDAP_USB_ASYNC_MAX_PENDING    16U
#define WDAP_USB_VENDOR_SYNC_ONLY  1

#if CONFIG_WDAP_USB_HID_FALLBACK
#if WDAP_USB_VENDOR_V2
#define WDAP_USB_ITF_VENDOR     0
#define WDAP_USB_ITF_HID        1
#define WDAP_USB_ITF_TOTAL      2
#define WDAP_USB_EP_VENDOR_OUT  0x01
#define WDAP_USB_EP_VENDOR_IN   0x81
#define WDAP_USB_EP_HID_OUT     0x02
#define WDAP_USB_EP_HID_IN      0x82
#define WDAP_USB_STR_HID        4
#define WDAP_USB_STR_VENDOR     5
#else
#define WDAP_USB_ITF_HID        0
#define WDAP_USB_ITF_VENDOR     1
#define WDAP_USB_ITF_TOTAL      1
#define WDAP_USB_EP_HID_OUT     0x01
#define WDAP_USB_EP_HID_IN      0x81
#define WDAP_USB_EP_VENDOR_OUT  0x02
#define WDAP_USB_EP_VENDOR_IN   0x82
#define WDAP_USB_STR_HID        4
#define WDAP_USB_STR_VENDOR     5
#endif
typedef enum {
    WDAP_USB_CH_VENDOR,
    WDAP_USB_CH_HID,
} wdap_usb_channel_t;
#else
#define WDAP_USB_ITF_VENDOR     0
#define WDAP_USB_ITF_TOTAL      1
#define WDAP_USB_EP_VENDOR_OUT  0x01
#define WDAP_USB_EP_VENDOR_IN   0x81
#define WDAP_USB_STR_VENDOR     4
typedef enum {
    WDAP_USB_CH_VENDOR,
} wdap_usb_channel_t;
#endif

#define WDAP_USB_STR_MANUF      1
#define WDAP_USB_STR_PRODUCT    2
#define WDAP_USB_STR_SERIAL     3
#define WDAP_USB_MS_VENDOR_CODE 0x20
#define WDAP_USB_MS_OS_20_LEN   0xB2

typedef struct {
    wdap_usb_channel_t channel;
    size_t len;
    uint8_t data[WDAP_DAP_PACKET_SIZE];
} wdap_usb_request_t;

static const char *TAG = "usb_dap_frontend";
static QueueHandle_t s_usb_req_queue;
#if WDAP_USB_DEBUG_LOG_LIMIT > 0U
static uint32_t s_req_log_count;
static uint32_t s_rsp_log_count;
#endif
static uint32_t s_async_pending;
static uint32_t s_async_errors;
static uint8_t s_async_batch[WDAP_ESPNOW_PAYLOAD_MTU];
static size_t s_async_batch_len;
static uint8_t s_async_batch_count;
static void enqueue_request(wdap_usb_channel_t channel, const uint8_t *data, size_t len);


#define WDAP_DAP_ID_INFO                 0x00U
#define WDAP_DAP_ID_HOST_STATUS          0x01U
#define WDAP_DAP_ID_CONNECT              0x02U
#define WDAP_DAP_ID_DISCONNECT           0x03U
#define WDAP_DAP_ID_TRANSFER_CONFIGURE   0x04U
#define WDAP_DAP_ID_TRANSFER             0x05U
#define WDAP_DAP_ID_TRANSFER_BLOCK       0x06U
#define WDAP_DAP_ID_TRANSFER_ABORT       0x07U
#define WDAP_DAP_ID_WRITE_ABORT          0x08U
#define WDAP_DAP_ID_DELAY                0x09U
#define WDAP_DAP_ID_RESET_TARGET         0x0AU
#define WDAP_DAP_ID_SWJ_PINS             0x10U
#define WDAP_DAP_ID_SWJ_CLOCK            0x11U
#define WDAP_DAP_ID_SWJ_SEQUENCE         0x12U
#define WDAP_DAP_ID_SWD_CONFIGURE        0x13U
#define WDAP_DAP_ID_JTAG_SEQUENCE        0x14U
#define WDAP_DAP_ID_JTAG_CONFIGURE       0x15U
#define WDAP_DAP_ID_JTAG_IDCODE          0x16U
#define WDAP_DAP_ID_SWD_SEQUENCE         0x1DU
#define WDAP_DAP_ID_EXECUTE_COMMANDS     0x7FU
#define WDAP_DAP_TRANSFER_RNW            (1U << 1)
#define WDAP_DAP_TRANSFER_MATCH_VALUE    (1U << 4)
#define WDAP_DAP_TRANSFER_MATCH_MASK     (1U << 5)
#define WDAP_DAP_SWJ_SEQUENCE_CLK        0xFFU
#define WDAP_DAP_SWD_SEQUENCE_CLK        0x3FU
#define WDAP_DAP_SWD_SEQUENCE_DIN        0x80U
#define WDAP_DAP_JTAG_SEQUENCE_TCK       0x3FU
#define WDAP_DAP_EXPECT_INVALID          ((size_t)-1)
#define WDAP_VENDOR_RX_GAP_RESET_MS      25U

static uint8_t s_vendor_rx_buf[WDAP_DAP_PACKET_SIZE];
static size_t s_vendor_rx_len;
static TickType_t s_vendor_rx_last_tick;

static size_t dap_single_expected_len(const uint8_t *data, size_t len, uint8_t depth);

static size_t dap_transfer_expected_len(const uint8_t *data, size_t len)
{
    if (len < 3U) {
        return 0U;
    }
    size_t offset = 3U;
    uint8_t count = data[2];
    for (uint8_t index = 0; index < count; ++index) {
        if (len < offset + 1U) {
            return 0U;
        }
        uint8_t request = data[offset++];
        if ((request & WDAP_DAP_TRANSFER_RNW) != 0U) {
            if ((request & WDAP_DAP_TRANSFER_MATCH_VALUE) != 0U) {
                offset += 4U;
            }
        } else {
            offset += 4U;
        }
        if (offset > WDAP_DAP_PACKET_SIZE) {
            return WDAP_DAP_EXPECT_INVALID;
        }
        if (len < offset) {
            return 0U;
        }
    }
    return offset;
}

static size_t dap_transfer_block_expected_len(const uint8_t *data, size_t len)
{
    if (len < 5U) {
        return 0U;
    }
    uint16_t count = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
    size_t expected = 5U;
    if ((data[4] & WDAP_DAP_TRANSFER_RNW) == 0U) {
        expected += (size_t)count * 4U;
    }
    return expected <= WDAP_DAP_PACKET_SIZE ? expected : WDAP_DAP_EXPECT_INVALID;
}

static size_t dap_swj_sequence_expected_len(const uint8_t *data, size_t len)
{
    if (len < 2U) {
        return 0U;
    }
    uint32_t bit_count = data[1];
    if (bit_count == 0U) {
        bit_count = 256U;
    }
    size_t expected = 2U + ((bit_count + 7U) >> 3);
    return expected <= WDAP_DAP_PACKET_SIZE ? expected : WDAP_DAP_EXPECT_INVALID;
}

static size_t dap_swd_sequence_expected_len(const uint8_t *data, size_t len)
{
    if (len < 2U) {
        return 0U;
    }
    size_t offset = 2U;
    uint8_t sequence_count = data[1];
    for (uint8_t index = 0; index < sequence_count; ++index) {
        if (len < offset + 1U) {
            return 0U;
        }
        uint8_t info = data[offset++];
        uint32_t bit_count = info & WDAP_DAP_SWD_SEQUENCE_CLK;
        if (bit_count == 0U) {
            bit_count = 64U;
        }
        if ((info & WDAP_DAP_SWD_SEQUENCE_DIN) == 0U) {
            offset += (bit_count + 7U) >> 3;
        }
        if (offset > WDAP_DAP_PACKET_SIZE) {
            return WDAP_DAP_EXPECT_INVALID;
        }
        if (len < offset) {
            return 0U;
        }
    }
    return offset;
}

static size_t dap_jtag_sequence_expected_len(const uint8_t *data, size_t len)
{
    if (len < 2U) {
        return 0U;
    }
    size_t offset = 2U;
    uint8_t sequence_count = data[1];
    for (uint8_t index = 0; index < sequence_count; ++index) {
        if (len < offset + 1U) {
            return 0U;
        }
        uint8_t info = data[offset++];
        uint32_t bit_count = info & WDAP_DAP_JTAG_SEQUENCE_TCK;
        if (bit_count == 0U) {
            bit_count = 64U;
        }
        offset += (bit_count + 7U) >> 3;
        if (offset > WDAP_DAP_PACKET_SIZE) {
            return WDAP_DAP_EXPECT_INVALID;
        }
        if (len < offset) {
            return 0U;
        }
    }
    return offset;
}

static size_t dap_execute_commands_expected_len(const uint8_t *data, size_t len, uint8_t depth)
{
    if (len < 2U) {
        return 0U;
    }
    size_t offset = 2U;
    uint8_t command_count = data[1];
    for (uint8_t index = 0; index < command_count; ++index) {
        size_t expected = dap_single_expected_len(data + offset, len - offset, (uint8_t)(depth + 1U));
        if (expected == 0U || expected == WDAP_DAP_EXPECT_INVALID) {
            return expected;
        }
        offset += expected;
        if (offset > WDAP_DAP_PACKET_SIZE) {
            return WDAP_DAP_EXPECT_INVALID;
        }
    }
    return offset;
}

static size_t dap_single_expected_len(const uint8_t *data, size_t len, uint8_t depth)
{
    if (len == 0U) {
        return 0U;
    }
    if (depth > 2U) {
        return WDAP_DAP_EXPECT_INVALID;
    }
    switch (data[0]) {
    case WDAP_DAP_ID_INFO:
    case WDAP_DAP_ID_CONNECT:
    case WDAP_DAP_ID_SWD_CONFIGURE:
    case WDAP_DAP_ID_JTAG_IDCODE:
        return len >= 2U ? 2U : 0U;
    case WDAP_DAP_ID_HOST_STATUS:
    case WDAP_DAP_ID_DELAY:
        return len >= 3U ? 3U : 0U;
    case WDAP_DAP_ID_DISCONNECT:
    case WDAP_DAP_ID_TRANSFER_ABORT:
    case WDAP_DAP_ID_RESET_TARGET:
        return 1U;
    case WDAP_DAP_ID_TRANSFER_CONFIGURE:
    case WDAP_DAP_ID_WRITE_ABORT:
        return len >= 6U ? 6U : 0U;
    case WDAP_DAP_ID_TRANSFER:
        return dap_transfer_expected_len(data, len);
    case WDAP_DAP_ID_TRANSFER_BLOCK:
        return dap_transfer_block_expected_len(data, len);
    case WDAP_DAP_ID_SWJ_PINS:
        return len >= 7U ? 7U : 0U;
    case WDAP_DAP_ID_SWJ_CLOCK:
        return len >= 5U ? 5U : 0U;
    case WDAP_DAP_ID_SWJ_SEQUENCE:
        return dap_swj_sequence_expected_len(data, len);
    case WDAP_DAP_ID_SWD_SEQUENCE:
        return dap_swd_sequence_expected_len(data, len);
    case WDAP_DAP_ID_JTAG_SEQUENCE:
        return dap_jtag_sequence_expected_len(data, len);
    case WDAP_DAP_ID_JTAG_CONFIGURE:
        if (len < 2U) {
            return 0U;
        }
        return (2U + data[1]) <= WDAP_DAP_PACKET_SIZE ? (2U + data[1]) : WDAP_DAP_EXPECT_INVALID;
    case WDAP_DAP_ID_EXECUTE_COMMANDS:
        return dap_execute_commands_expected_len(data, len, depth);
    default:
        return 1U;
    }
}

static bool bytes_all_zero(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0U) {
        return false;
    }
    for (size_t index = 0; index < len; ++index) {
        if (data[index] != 0U) {
            return false;
        }
    }
    return true;
}

static void vendor_rx_reset(void)
{
    s_vendor_rx_len = 0U;
}

#if !WDAP_USB_VENDOR_V2
static void vendor_rx_consume(size_t consumed)
{
    if (consumed >= s_vendor_rx_len) {
        s_vendor_rx_len = 0U;
        return;
    }
    memmove(s_vendor_rx_buf, s_vendor_rx_buf + consumed, s_vendor_rx_len - consumed);
    s_vendor_rx_len -= consumed;
}
#endif

static void vendor_rx_feed(const uint8_t *data, size_t len)
{
#if WDAP_USB_VENDOR_V2
    TickType_t now = xTaskGetTickCount();
    if (s_vendor_rx_len > 0U && (now - s_vendor_rx_last_tick) > pdMS_TO_TICKS(WDAP_VENDOR_RX_GAP_RESET_MS)) {
        ESP_LOGW(TAG, "Vendor RX partial packet reset len=%u", (unsigned)s_vendor_rx_len);
        vendor_rx_reset();
    }
    s_vendor_rx_last_tick = now;

    while (len > 0U) {
        size_t room = sizeof(s_vendor_rx_buf) - s_vendor_rx_len;
        if (room == 0U) {
            enqueue_request(WDAP_USB_CH_VENDOR, s_vendor_rx_buf, s_vendor_rx_len);
            vendor_rx_reset();
            room = sizeof(s_vendor_rx_buf);
        }

        size_t copy_len = len < room ? len : room;
        memcpy(s_vendor_rx_buf + s_vendor_rx_len, data, copy_len);
        s_vendor_rx_len += copy_len;
        data += copy_len;
        len -= copy_len;

        bool short_packet = (copy_len % WDAP_USB_BULK_EP_SIZE) != 0U;
        if (s_vendor_rx_len >= WDAP_DAP_PACKET_SIZE || short_packet) {
            if (s_vendor_rx_len > 0U && !bytes_all_zero(s_vendor_rx_buf, s_vendor_rx_len)) {
                enqueue_request(WDAP_USB_CH_VENDOR, s_vendor_rx_buf, s_vendor_rx_len);
            }
            vendor_rx_reset();
        }
    }
#else
    TickType_t now = xTaskGetTickCount();
    if (s_vendor_rx_len > 0U && (now - s_vendor_rx_last_tick) > pdMS_TO_TICKS(WDAP_VENDOR_RX_GAP_RESET_MS)) {
        ESP_LOGW(TAG, "Vendor RX partial packet reset len=%u", (unsigned)s_vendor_rx_len);
        vendor_rx_reset();
    }
    s_vendor_rx_last_tick = now;

    while (len > 0U) {
        if (s_vendor_rx_len == 0U && bytes_all_zero(data, len)) {
            break;
        }
        size_t room = sizeof(s_vendor_rx_buf) - s_vendor_rx_len;
        if (room == 0U) {
            ESP_LOGW(TAG, "Vendor RX overflow, dropping partial packet len=%u", (unsigned)s_vendor_rx_len);
            vendor_rx_reset();
            room = sizeof(s_vendor_rx_buf);
        }
        size_t copy_len = len < room ? len : room;
        memcpy(s_vendor_rx_buf + s_vendor_rx_len, data, copy_len);
        s_vendor_rx_len += copy_len;
        data += copy_len;
        len -= copy_len;

        while (s_vendor_rx_len > 0U) {
            size_t expected = dap_single_expected_len(s_vendor_rx_buf, s_vendor_rx_len, 0U);
            if (expected == 0U) {
                break;
            }
            if (expected == WDAP_DAP_EXPECT_INVALID || expected > sizeof(s_vendor_rx_buf)) {
                ESP_LOGW(TAG, "Vendor RX invalid packet cmd=0x%02x len=%u", (unsigned)s_vendor_rx_buf[0], (unsigned)s_vendor_rx_len);
                vendor_rx_reset();
                break;
            }
            if (s_vendor_rx_len < expected) {
                break;
            }
            enqueue_request(WDAP_USB_CH_VENDOR, s_vendor_rx_buf, expected);
            vendor_rx_consume(expected);
            if (s_vendor_rx_len > 0U && bytes_all_zero(s_vendor_rx_buf, s_vendor_rx_len)) {
                vendor_rx_reset();
                break;
            }
        }
    }
#endif
}

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = WDAP_USB_VENDOR_V2 ? 0x0210 : 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = WDAP_USB_VID,
    .idProduct = WDAP_USB_PID,
    .bcdDevice = WDAP_USB_BCD,
    .iManufacturer = WDAP_USB_STR_MANUF,
    .iProduct = WDAP_USB_STR_PRODUCT,
    .iSerialNumber = WDAP_USB_STR_SERIAL,
    .bNumConfigurations = 1,
};

#if CONFIG_WDAP_USB_HID_FALLBACK
static const uint8_t s_hid_report_desc[] = {
    0x06, 0x00, 0xff,
    0x09, 0x01,
    0xa1, 0x01,
    0x15, 0x00,
    0x26, 0xff, 0x00,
    0x75, 0x08,
    0x96, (uint8_t)(WDAP_DAP_PACKET_SIZE & 0xff), (uint8_t)(WDAP_DAP_PACKET_SIZE >> 8),
    0x09, 0x01,
    0x81, 0x02,
    0x96, (uint8_t)(WDAP_DAP_PACKET_SIZE & 0xff), (uint8_t)(WDAP_DAP_PACKET_SIZE >> 8),
    0x09, 0x01,
    0x91, 0x02,
    0xc0,
};
#if WDAP_USB_VENDOR_V2
#define WDAP_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_VENDOR_DESC_LEN)
#else
#define WDAP_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)
#endif
#else
#define WDAP_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)
#endif

static const uint8_t s_fs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, WDAP_USB_ITF_TOTAL, 0, WDAP_CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
#if WDAP_USB_VENDOR_V2
    TUD_VENDOR_DESCRIPTOR(WDAP_USB_ITF_VENDOR, WDAP_USB_STR_VENDOR,
                          WDAP_USB_EP_VENDOR_OUT, WDAP_USB_EP_VENDOR_IN, WDAP_USB_BULK_EP_SIZE),
#endif
#if CONFIG_WDAP_USB_HID_FALLBACK
    TUD_HID_INOUT_DESCRIPTOR(WDAP_USB_ITF_HID, WDAP_USB_STR_HID, HID_ITF_PROTOCOL_NONE,
                             sizeof(s_hid_report_desc), WDAP_USB_EP_HID_OUT, WDAP_USB_EP_HID_IN,
                             64, 1),
#endif
};

#define WDAP_BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
static const uint8_t s_bos_desc[] = {
    TUD_BOS_DESCRIPTOR(WDAP_BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(WDAP_USB_MS_OS_20_LEN, WDAP_USB_MS_VENDOR_CODE),
};

static const uint8_t s_ms_os_20_desc[] = {
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(WDAP_USB_MS_OS_20_LEN),
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0, U16_TO_U8S_LE(WDAP_USB_MS_OS_20_LEN - 0x0A),
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), WDAP_USB_ITF_VENDOR, 0, U16_TO_U8S_LE(WDAP_USB_MS_OS_20_LEN - 0x0A - 0x08),
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(WDAP_USB_MS_OS_20_LEN - 0x0A - 0x08 - 0x08 - 0x14), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,
    'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050),
    '{', 0x00, 'C', 0x00, 'D', 0x00, 'B', 0x00, '3', 0x00, 'B', 0x00, '5', 0x00, 'A', 0x00, 'D', 0x00, '-', 0x00,
    '2', 0x00, '9', 0x00, '3', 0x00, 'B', 0x00, '-', 0x00, '4', 0x00, '6', 0x00, '6', 0x00, '3', 0x00, '-', 0x00,
    'A', 0x00, 'A', 0x00, '3', 0x00, '6', 0x00, '-', 0x00, '1', 0x00, 'A', 0x00, 'A', 0x00, 'E', 0x00, '4', 0x00,
    '6', 0x00, '4', 0x00, '6', 0x00, '3', 0x00, '7', 0x00, '7', 0x00, '6', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,
};

TU_VERIFY_STATIC(sizeof(s_ms_os_20_desc) == WDAP_USB_MS_OS_20_LEN, "bad MS OS descriptor length");

static const char *s_string_desc[] = {
    (const char[]){0x09, 0x04},
    WDAP_USB_MANUFACTURER,
    WDAP_USB_PRODUCT,
    WDAP_USB_SERIAL,
#if CONFIG_WDAP_USB_HID_FALLBACK
    WDAP_USB_HID_INTERFACE,
#if WDAP_USB_VENDOR_V2
    WDAP_USB_VENDOR_INTERFACE,
#endif
#else
    WDAP_USB_VENDOR_INTERFACE,
#endif
};

uint8_t const *tud_descriptor_bos_cb(void)
{
    return WDAP_USB_VENDOR_V2 ? s_bos_desc : NULL;
}

#if CONFIG_WDAP_USB_HID_FALLBACK
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_desc;
}
#endif

static void enqueue_request(wdap_usb_channel_t channel, const uint8_t *data, size_t len)
{
    if (s_usb_req_queue == NULL || data == NULL || len == 0) {
        return;
    }
    size_t request_len = len > WDAP_DAP_PACKET_SIZE ? WDAP_DAP_PACKET_SIZE : len;
    size_t expected_len = dap_single_expected_len(data, request_len, 0U);
    if (expected_len > 0U && expected_len != WDAP_DAP_EXPECT_INVALID && expected_len < request_len &&
        bytes_all_zero(data + expected_len, request_len - expected_len)) {
        request_len = expected_len;
    }
    wdap_usb_request_t request = {
        .channel = channel,
        .len = request_len,
    };
    memcpy(request.data, data, request.len);
#if WDAP_USB_DEBUG_LOG_LIMIT > 0U
    if (s_req_log_count < WDAP_USB_DEBUG_LOG_LIMIT) {
        ESP_LOGI(TAG, "USB req ch=%u len=%u cmd=0x%02x b1=%02x b2=%02x b3=%02x b4=%02x b5=%02x b6=%02x b7=%02x",
                 (unsigned)channel, (unsigned)request.len, (unsigned)request.data[0],
                 request.len > 1U ? request.data[1] : 0U, request.len > 2U ? request.data[2] : 0U,
                 request.len > 3U ? request.data[3] : 0U, request.len > 4U ? request.data[4] : 0U,
                 request.len > 5U ? request.data[5] : 0U, request.len > 6U ? request.data[6] : 0U,
                 request.len > 7U ? request.data[7] : 0U);
        s_req_log_count++;
    }
#endif
    if (xQueueSend(s_usb_req_queue, &request, 0) != pdTRUE) {
        ESP_LOGW(TAG, "USB request queue full");
    }
}

#if CONFIG_WDAP_USB_HID_FALLBACK
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    if (report_type == HID_REPORT_TYPE_OUTPUT) {
        enqueue_request(WDAP_USB_CH_HID, buffer, bufsize > WDAP_DAP_PACKET_SIZE ? WDAP_DAP_PACKET_SIZE : bufsize);
    }
}
#endif

#if (TUSB_VERSION_MINOR >= 17)
void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    vendor_rx_feed(buffer, bufsize);
#if CFG_TUD_VENDOR_RX_BUFSIZE > 0
    tud_vendor_n_read_flush(itf);
#endif
}
#else
void tud_vendor_rx_cb(uint8_t itf)
{
    uint8_t buffer[WDAP_DAP_PACKET_SIZE];
    uint32_t available = tud_vendor_n_available(itf);
    while (available > 0U) {
        uint32_t read_len = tud_vendor_n_read(itf, buffer, available > sizeof(buffer) ? sizeof(buffer) : available);
        if (read_len == 0U) {
            break;
        }
        vendor_rx_feed(buffer, read_len);
        available = tud_vendor_n_available(itf);
    }
}
#endif

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    if (!WDAP_USB_VENDOR_V2) {
        return false;
    }
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == WDAP_USB_MS_VENDOR_CODE && request->wIndex == 7) {
        uint16_t total_len;
        memcpy(&total_len, s_ms_os_20_desc + 8, sizeof(total_len));
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)s_ms_os_20_desc, total_len);
    }
    return false;
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event == NULL) {
        return;
    }
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        ESP_LOGI(TAG, "USB mounted/configured");
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        ESP_LOGW(TAG, "USB unmounted");
    }
}

static bool dap_padding_is_zero(const uint8_t *request, size_t expected_len, size_t request_len)
{
    if (expected_len > request_len) {
        return false;
    }
    for (size_t index = expected_len; index < request_len; ++index) {
        if (request[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool dap_make_async_write_response(const uint8_t *request, size_t request_len, uint8_t *response, size_t *response_len)
{
#if WDAP_USB_ASYNC_WRITE_PIPELINE
    if (request == NULL || response == NULL || response_len == NULL || request_len == 0U) {
        return false;
    }
    size_t expected_len = dap_single_expected_len(request, request_len, 0U);
    if (expected_len == 0U || expected_len == WDAP_DAP_EXPECT_INVALID || expected_len > request_len ||
        !dap_padding_is_zero(request, expected_len, request_len)) {
        return false;
    }
    if (request[0] == WDAP_DAP_ID_TRANSFER) {
        if (expected_len < 3U) {
            return false;
        }
        uint8_t count = request[2];
        size_t offset = 3U;
        for (uint8_t index = 0; index < count; ++index) {
            if (expected_len < offset + 1U) {
                return false;
            }
            uint8_t transfer_request = request[offset++];
            if ((transfer_request & WDAP_DAP_TRANSFER_RNW) != 0U) {
                return false;
            }
            if (expected_len < offset + 4U) {
                return false;
            }
            offset += 4U;
        }
        if (offset != expected_len) {
            return false;
        }
        response[0] = WDAP_DAP_ID_TRANSFER;
        response[1] = count;
        response[2] = 0x01U;
        *response_len = 3U;
        return true;
    }
    if (request[0] == WDAP_DAP_ID_TRANSFER_BLOCK) {
        if (expected_len < 5U || (request[4] & WDAP_DAP_TRANSFER_RNW) != 0U) {
            return false;
        }
        uint16_t count = (uint16_t)request[2] | ((uint16_t)request[3] << 8);
        size_t expected_block_len = 5U + ((size_t)count * 4U);
        if (expected_block_len != expected_len) {
            return false;
        }
        response[0] = WDAP_DAP_ID_TRANSFER_BLOCK;
        response[1] = request[2];
        response[2] = request[3];
        response[3] = 0x01U;
        *response_len = 4U;
        return true;
    }
#else
    (void)request;
    (void)request_len;
    (void)response;
    (void)response_len;
#endif
    return false;
}

static void async_batch_reset(void)
{
    s_async_batch_len = 1U;
    s_async_batch_count = 0U;
    s_async_batch[0] = 0U;
}

static esp_err_t async_batch_flush(uint32_t timeout_ms)
{
#if WDAP_USB_ASYNC_BATCH_NORESP
    if (s_async_batch_count == 0U) {
        return ESP_OK;
    }
    esp_err_t err = wdap_transport_send_message(WDAP_PKT_DAP_BATCH_NORESP, s_async_batch, s_async_batch_len, timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "async DAP batch send failed count=%u len=%u err=%s", (unsigned)s_async_batch_count,
                 (unsigned)s_async_batch_len, esp_err_to_name(err));
    }
    async_batch_reset();
    return err;
#else
    (void)timeout_ms;
    return ESP_OK;
#endif
}

static esp_err_t async_batch_append(const uint8_t *request, size_t request_len, uint32_t timeout_ms)
{
#if WDAP_USB_ASYNC_BATCH_NORESP
    if (s_async_batch_len == 0U) {
        async_batch_reset();
    }
    if (request_len == 0U || request_len > UINT8_MAX) {
        return wdap_transport_send_message(WDAP_PKT_DAP_REQ_NORESP, request, request_len, timeout_ms);
    }
    size_t needed = 1U + request_len;
    if (needed > sizeof(s_async_batch)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_async_batch_count >= WDAP_USB_ASYNC_BATCH_MAX_CMDS || s_async_batch_len + needed > sizeof(s_async_batch)) {
        ESP_RETURN_ON_ERROR(async_batch_flush(timeout_ms), TAG, "async DAP batch preflush failed");
    }
    s_async_batch[s_async_batch_len++] = (uint8_t)request_len;
    memcpy(s_async_batch + s_async_batch_len, request, request_len);
    s_async_batch_len += request_len;
    s_async_batch_count++;
    s_async_batch[0] = s_async_batch_count;
    if (s_async_batch_count >= WDAP_USB_ASYNC_BATCH_MAX_CMDS) {
        ESP_RETURN_ON_ERROR(async_batch_flush(timeout_ms), TAG, "async DAP batch full flush failed");
    }
    return ESP_OK;
#else
    (void)request;
    (void)request_len;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
static bool dap_async_response_ok(const wdap_message_t *message)
{
    if (message == NULL || message->type != WDAP_PKT_DAP_RSP || message->len == 0U) {
        return false;
    }
    if (message->payload[0] == WDAP_DAP_ID_TRANSFER) {
        return message->len >= 3U && message->payload[2] == 0x01U;
    }
    if (message->payload[0] == WDAP_DAP_ID_TRANSFER_BLOCK) {
        return message->len >= 4U && message->payload[3] == 0x01U;
    }
    return true;
}

static bool drain_one_async_response(uint32_t timeout_ms)
{
    if (s_async_pending == 0U) {
        return true;
    }
    wdap_message_t async_response;
    esp_err_t err = wdap_transport_recv_message(&async_response, timeout_ms);
    if (err != ESP_OK || !dap_async_response_ok(&async_response)) {
        s_async_errors++;
        ESP_LOGE(TAG, "async DAP write failed err=%s type=%u len=%u cmd=0x%02x status=0x%02x",
                 esp_err_to_name(err), err == ESP_OK ? (unsigned)async_response.type : 0U,
                 err == ESP_OK ? (unsigned)async_response.len : 0U,
                 (err == ESP_OK && async_response.len > 0U) ? async_response.payload[0] : 0U,
                 (err == ESP_OK && async_response.len > 3U) ? async_response.payload[3] : 0U);
    }
    s_async_pending--;
    return err == ESP_OK;
}

static bool flush_async_writes(uint32_t timeout_ms)
{
    bool ok = true;
    while (s_async_pending > 0U) {
        ok = drain_one_async_response(timeout_ms) && ok;
    }
    return ok && s_async_errors == 0U;
}

static void write_usb_response(wdap_usb_channel_t channel, const uint8_t *payload, size_t payload_len)
{
    uint8_t out[WDAP_DAP_PACKET_SIZE] = {0};
    size_t out_len = payload_len > sizeof(out) ? sizeof(out) : payload_len;
    if (payload != NULL && out_len > 0U) {
        memcpy(out, payload, out_len);
    }
#if WDAP_USB_DEBUG_LOG_LIMIT > 0U
    if (s_rsp_log_count < WDAP_USB_DEBUG_LOG_LIMIT) {
        ESP_LOGI(TAG, "USB rsp ch=%u payload_len=%u out_len=%u cmd=0x%02x b1=%02x b2=%02x b3=%02x b4=%02x b5=%02x b6=%02x b7=%02x",
                 (unsigned)channel, (unsigned)payload_len, (unsigned)out_len, out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
        s_rsp_log_count++;
    }
#endif
#if CONFIG_WDAP_USB_HID_FALLBACK
    if (channel == WDAP_USB_CH_HID) {
        for (int retry = 0; retry < 50 && !tud_hid_n_ready(0); ++retry) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (!tud_hid_n_report(0, 0, out, WDAP_DAP_PACKET_SIZE)) {
            ESP_LOGW(TAG, "HID response write failed");
        }
        return;
    }
#endif
    const uint8_t *write_ptr = out;
    size_t vendor_write_len = out_len;
    size_t remaining = vendor_write_len;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(WDAP_TRANSFER_TIMEOUT_MS);
    while (remaining > 0U && xTaskGetTickCount() < deadline) {
        uint32_t written = tud_vendor_n_write(0, write_ptr, remaining);
        if (written > 0U) {
            write_ptr += written;
            remaining -= written;
            tud_vendor_n_write_flush(0);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    if (remaining > 0U) {
        ESP_LOGW(TAG, "Vendor response short write %u/%u", (unsigned)(vendor_write_len - remaining), (unsigned)vendor_write_len);
    }
}

static void usb_worker_task(void *arg)
{
    (void)arg;
    wdap_usb_request_t request;
    wdap_message_t response;

    while (xQueueReceive(s_usb_req_queue, &request, portMAX_DELAY) == pdTRUE) {
        uint8_t fake_response[WDAP_DAP_PACKET_SIZE] = {0};
        size_t fake_response_len = 0U;
        bool allow_async = !(WDAP_USB_VENDOR_V2 && WDAP_USB_VENDOR_SYNC_ONLY && request.channel == WDAP_USB_CH_VENDOR);
        if (allow_async && dap_make_async_write_response(request.data, request.len, fake_response, &fake_response_len)) {
#if WDAP_USB_ASYNC_NO_RESPONSE
#if WDAP_USB_ASYNC_BATCH_NORESP
            esp_err_t async_err = async_batch_append(request.data, request.len, WDAP_TRANSFER_TIMEOUT_MS);
#else
            esp_err_t async_err = wdap_transport_send_message(WDAP_PKT_DAP_REQ_NORESP, request.data, request.len, WDAP_TRANSFER_TIMEOUT_MS);
#endif
            if (async_err == ESP_OK) {
                write_usb_response(request.channel, fake_response, fake_response_len);
                continue;
            }
            ESP_LOGW(TAG, "async no-response DAP send failed, falling back: %s", esp_err_to_name(async_err));
#else
            esp_err_t async_err = wdap_transport_send_message(WDAP_PKT_DAP_REQ, request.data, request.len, WDAP_TRANSFER_TIMEOUT_MS);
            if (async_err == ESP_OK) {
                s_async_pending++;
                write_usb_response(request.channel, fake_response, fake_response_len);
                while (s_async_pending >= WDAP_USB_ASYNC_MAX_PENDING) {
                    drain_one_async_response(WDAP_TRANSFER_TIMEOUT_MS);
                }
                while (s_async_pending > 0U) {
                    wdap_message_t opportunistic_response;
                    esp_err_t drain_err = wdap_transport_recv_message(&opportunistic_response, 0);
                    if (drain_err != ESP_OK) {
                        break;
                    }
                    if (!dap_async_response_ok(&opportunistic_response)) {
                        s_async_errors++;
                        ESP_LOGE(TAG, "async DAP write reported bad status len=%u", (unsigned)opportunistic_response.len);
                    }
                    s_async_pending--;
                }
                continue;
            }
            ESP_LOGW(TAG, "async DAP send failed, flushing and falling back: %s", esp_err_to_name(async_err));
#endif
        }

        if (async_batch_flush(WDAP_TRANSFER_TIMEOUT_MS) != ESP_OK) {
            ESP_LOGE(TAG, "async write batch flush failed before synchronous DAP command");
        }
        if (!flush_async_writes(WDAP_TRANSFER_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "async write pipeline flush reported errors=%lu", (unsigned long)s_async_errors);
            s_async_errors = 0U;
        }

        esp_err_t err = wdap_transport_send_message(WDAP_PKT_DAP_REQ, request.data, request.len, WDAP_TRANSFER_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "DAP request send failed: %s", esp_err_to_name(err));
            continue;
        }
        err = wdap_transport_recv_message(&response, WDAP_TRANSFER_TIMEOUT_MS);
        if (err != ESP_OK || response.type != WDAP_PKT_DAP_RSP) {
            ESP_LOGE(TAG, "DAP response timeout/error: %s type=%u", esp_err_to_name(err), err == ESP_OK ? (unsigned)response.type : 0U);
            continue;
        }
        write_usb_response(request.channel, response.payload, response.len);
    }
}

esp_err_t usb_dap_frontend_init(void)
{
    s_usb_req_queue = xQueueCreate(16, sizeof(wdap_usb_request_t));
    if (s_usb_req_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.task.size = 6144;
    tusb_cfg.task.priority = 20;
    tusb_cfg.task.xCoreID = WDAP_USB_TASK_CORE_ID;
    tusb_cfg.descriptor.device = &s_device_desc;
    tusb_cfg.descriptor.string = s_string_desc;
    tusb_cfg.descriptor.string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]);
    tusb_cfg.descriptor.full_speed_config = s_fs_config_desc;
    tusb_cfg.descriptor.high_speed_config = s_fs_config_desc;
    tusb_cfg.event_cb = usb_event_cb;

    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb_driver_install failed");
    BaseType_t ok = xTaskCreatePinnedToCore(usb_worker_task, "usb_dap_worker", 8192, NULL, 21, NULL, WDAP_USB_TASK_CORE_ID);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB CMSIS-DAP HID optimized initialized, dap_packet=%u hid_fallback=%u",
             (unsigned)WDAP_DAP_PACKET_SIZE, (unsigned)CONFIG_WDAP_USB_HID_FALLBACK);
    return ESP_OK;
}