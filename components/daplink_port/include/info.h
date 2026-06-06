#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void info_init(void);
void info_set_uuid_target(uint32_t *uuid_data);
void info_crc_compute(void);
const char *info_get_unique_id(void);
const char *info_get_board_id(void);
const char *info_get_host_id(void);
const char *info_get_target_id(void);
const char *info_get_hic_id(void);
const char *info_get_version(void);
const char *info_get_mac(void);
const char *info_get_unique_id_string_descriptor(void);
bool info_get_bootloader_present(void);
bool info_get_interface_present(void);
bool info_get_config_admin_present(void);
bool info_get_config_user_present(void);
uint32_t info_get_crc_bootloader(void);
uint32_t info_get_crc_interface(void);
uint32_t info_get_crc_config_user(void);
uint32_t info_get_bootloader_version(void);
uint32_t info_get_interface_version(void);

#ifdef __cplusplus
}
#endif
