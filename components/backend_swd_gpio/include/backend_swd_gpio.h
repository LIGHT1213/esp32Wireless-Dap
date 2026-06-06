#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t backend_swd_gpio_init(void);
void backend_swd_gpio_port_swd_setup(void);
void backend_swd_gpio_port_off(void);
void backend_swd_gpio_swclk_write(uint32_t bit);
uint32_t backend_swd_gpio_swclk_read(void);
void backend_swd_gpio_swdio_write(uint32_t bit);
uint32_t backend_swd_gpio_swdio_read(void);
void backend_swd_gpio_swdio_output_enable(void);
void backend_swd_gpio_swdio_output_disable(void);
void backend_swd_gpio_nreset_write(uint32_t bit);
uint32_t backend_swd_gpio_nreset_read(void);

#ifdef __cplusplus
}
#endif
