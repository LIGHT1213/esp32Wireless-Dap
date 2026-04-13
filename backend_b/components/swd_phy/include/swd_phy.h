#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int swclk_gpio;
    int swdio_gpio;
    uint32_t clock_hz;
} swd_phy_config_t;

esp_err_t swd_phy_init(const swd_phy_config_t *config);
esp_err_t swd_phy_set_clock(uint32_t hz);
esp_err_t swd_phy_line_reset(void);
esp_err_t swd_phy_jtag_to_swd(void);
esp_err_t swd_phy_read_dp(uint8_t addr, uint32_t *value, uint8_t *ack);
esp_err_t swd_phy_write_dp(uint8_t addr, uint32_t value, uint8_t *ack);
esp_err_t swd_phy_read_ap(uint8_t addr, uint32_t *value, uint8_t *ack);
esp_err_t swd_phy_write_ap(uint8_t addr, uint32_t value, uint8_t *ack);
