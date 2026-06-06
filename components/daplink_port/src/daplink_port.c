#include "daplink_port.h"

#include <stdbool.h>
#include <string.h>
#include "DAP_config.h"
#include "DAP.h"
#include "board_config.h"
#include "esp_log.h"
#include "target_board.h"

static const char *TAG = "daplink_port";
static const target_cfg_t s_target_cfg = {
    .target_vendor = "Unknown",
    .target_part_number = "SWD Target",
};

const board_info_t g_board_info = {
    .target_cfg = &s_target_cfg,
    .board_vendor = WDAP_USB_MANUFACTURER,
    .board_name = WDAP_USB_PRODUCT,
};

esp_err_t daplink_port_init(void)
{
    DAP_Setup();
    ESP_LOGI(TAG, "DAPLink CMSIS-DAP core initialized, packet_size=%d packet_count=%d", DAP_PACKET_SIZE, DAP_PACKET_COUNT);
    return ESP_OK;
}

size_t daplink_port_process(const uint8_t *request, size_t request_len, uint8_t *response, size_t response_capacity)
{
    if (request == NULL || response == NULL || request_len == 0 || response_capacity == 0) {
        return 0;
    }

    uint8_t capped_request[WDAP_DAP_PACKET_SIZE];
    if (request_len >= 5U && request[0] == ID_DAP_SWJ_Clock) {
        uint32_t clock = ((uint32_t)request[1]) |
                         ((uint32_t)request[2] << 8) |
                         ((uint32_t)request[3] << 16) |
                         ((uint32_t)request[4] << 24);
        if (clock > WDAP_MAX_SWJ_CLOCK_HZ) {
            size_t copy_len = request_len > sizeof(capped_request) ? sizeof(capped_request) : request_len;
            memcpy(capped_request, request, copy_len);
            clock = WDAP_MAX_SWJ_CLOCK_HZ;
            capped_request[1] = (uint8_t)(clock >> 0);
            capped_request[2] = (uint8_t)(clock >> 8);
            capped_request[3] = (uint8_t)(clock >> 16);
            capped_request[4] = (uint8_t)(clock >> 24);
            request = capped_request;
            request_len = copy_len;
        }
    }

    memset(response, 0, response_capacity);
    uint32_t result = DAP_ExecuteCommand(request, response);
    size_t response_len = (size_t)(result & 0xFFFFU);
    if (response_len > response_capacity) {
        response_len = response_capacity;
    }
    return response_len;
}

void info_init(void) {}
void info_set_uuid_target(uint32_t *uuid_data) { (void)uuid_data; }
void info_crc_compute(void) {}
const char *info_get_unique_id(void) { return WDAP_USB_SERIAL; }
const char *info_get_board_id(void) { return "WDAP"; }
const char *info_get_host_id(void) { return "ESP32S3"; }
const char *info_get_target_id(void) { return "00000000000000000000000000000000"; }
const char *info_get_hic_id(void) { return "ESP32S3"; }
const char *info_get_version(void) { return "0001"; }
const char *info_get_mac(void) { return "206EF1D602D0"; }
const char *info_get_unique_id_string_descriptor(void) { return WDAP_USB_SERIAL; }
bool info_get_bootloader_present(void) { return false; }
bool info_get_interface_present(void) { return true; }
bool info_get_config_admin_present(void) { return false; }
bool info_get_config_user_present(void) { return false; }
uint32_t info_get_crc_bootloader(void) { return 0; }
uint32_t info_get_crc_interface(void) { return 0; }
uint32_t info_get_crc_config_user(void) { return 0; }
uint32_t info_get_bootloader_version(void) { return 0; }
uint32_t info_get_interface_version(void) { return 1; }
