#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t swd_engine_init(void);
esp_err_t swd_engine_set_frequency(uint32_t hz);
esp_err_t swd_engine_line_reset(uint8_t *ack);
esp_err_t swd_engine_swj_sequence(uint32_t count, const uint8_t *data);
esp_err_t swd_engine_target_reset(void);
esp_err_t swd_engine_target_reset_drive(bool asserted);
esp_err_t swd_engine_set_transfer_config(uint8_t idle_cycles, uint8_t turnaround, uint8_t data_phase);
esp_err_t swd_engine_target_halt(uint32_t *dhcsr);
esp_err_t swd_engine_read_dp_idcode(uint32_t *value, uint8_t *ack);
esp_err_t swd_engine_read_dp(uint8_t addr, uint32_t *value, uint8_t *ack);
esp_err_t swd_engine_write_dp(uint8_t addr, uint32_t value, uint8_t *ack);
esp_err_t swd_engine_read_ap(uint8_t addr, uint32_t *value, uint8_t *ack);
esp_err_t swd_engine_write_ap(uint8_t addr, uint32_t value, uint8_t *ack);
bool swd_engine_is_mock_mode(void);
uint32_t swd_engine_get_default_frequency(void);
uint32_t swd_engine_get_current_frequency(void);
uint32_t swd_engine_get_capabilities(void);
