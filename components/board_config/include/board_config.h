#pragma once

#include <stdint.h>
#include "sdkconfig.h"

#define WDAP_ESPNOW_CHANNEL              CONFIG_WDAP_ESPNOW_CHANNEL
#define WDAP_ESPNOW_PAYLOAD_MTU          CONFIG_WDAP_ESPNOW_PAYLOAD_MTU
#define WDAP_TRANSFER_TIMEOUT_MS         CONFIG_WDAP_TRANSFER_TIMEOUT_MS
#define WDAP_ESPNOW_RETRY_COUNT          CONFIG_WDAP_ESPNOW_RETRY_COUNT

#define WDAP_DAP_PACKET_SIZE             CONFIG_WDAP_DAP_PACKET_SIZE
#define WDAP_DAP_PACKET_COUNT            CONFIG_WDAP_DAP_PACKET_COUNT

#define WDAP_FRONTEND_A_MAC_INIT         {0x20, 0x6e, 0xf1, 0xd6, 0x01, 0xa8}
#define WDAP_BACKEND_B_MAC_INIT          {0x20, 0x6e, 0xf1, 0xd6, 0x02, 0xd0}

#define WDAP_BACKEND_SWCLK_GPIO          5
#define WDAP_BACKEND_SWDIO_GPIO          4
#define WDAP_BACKEND_NRESET_GPIO         6
#define WDAP_BACKEND_UART_TX_GPIO        17
#define WDAP_BACKEND_UART_RX_GPIO        18
#define WDAP_BACKEND_UART_BAUDRATE       115200

#define WDAP_DEFAULT_SWJ_CLOCK_HZ        4000000U
#define WDAP_MAX_SWJ_CLOCK_HZ            8000000U

#define WDAP_USB_VID                     0x0D28
#define WDAP_USB_PID                     0x0204
#define WDAP_USB_BCD                     0x0101
#define WDAP_USB_MANUFACTURER            "OpenAI"
#define WDAP_USB_PRODUCT                 "Wireless CMSIS-DAP"
#define WDAP_USB_SERIAL                  "WDAP-S3-0001"
#define WDAP_USB_HID_INTERFACE           "WDAP HID Fallback"
#define WDAP_USB_VENDOR_INTERFACE        "CMSIS-DAP v2"

#if CONFIG_FREERTOS_UNICORE
#define WDAP_TRANSPORT_TASK_CORE_ID      0
#define WDAP_USB_TASK_CORE_ID            0
#define WDAP_DAP_TASK_CORE_ID            0
#else
#define WDAP_TRANSPORT_TASK_CORE_ID      0
#define WDAP_USB_TASK_CORE_ID            1
#define WDAP_DAP_TASK_CORE_ID            1
#endif

static inline void wdap_board_get_peer_mac(uint8_t mac[6])
{
#if CONFIG_WDAP_ROLE_FRONTEND
    const uint8_t peer[6] = WDAP_BACKEND_B_MAC_INIT;
#else
    const uint8_t peer[6] = WDAP_FRONTEND_A_MAC_INIT;
#endif
    for (int i = 0; i < 6; ++i) {
        mac[i] = peer[i];
    }
}

static inline const char *wdap_board_role_name(void)
{
#if CONFIG_WDAP_ROLE_FRONTEND
    return "frontend_a";
#elif CONFIG_WDAP_ROLE_BACKEND
    return "backend_b";
#else
    return "unknown";
#endif
}
