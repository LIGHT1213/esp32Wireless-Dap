#include "backend_swd_gpio.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "backend_swd_gpio";

esp_err_t backend_swd_gpio_init(void)
{
    gpio_config_t swclk_cfg = {
        .pin_bit_mask = BIT64(WDAP_BACKEND_SWCLK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&swclk_cfg), TAG, "SWCLK gpio_config failed");

    gpio_config_t swdio_cfg = {
        .pin_bit_mask = BIT64(WDAP_BACKEND_SWDIO_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&swdio_cfg), TAG, "SWDIO gpio_config failed");

    gpio_config_t nreset_cfg = {
        .pin_bit_mask = BIT64(WDAP_BACKEND_NRESET_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&nreset_cfg), TAG, "nRESET gpio_config failed");

    uart_config_t uart_cfg = {
        .baud_rate = WDAP_BACKEND_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(UART_NUM_1, 1024, 1024, 0, NULL, 0), TAG, "uart_driver_install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(UART_NUM_1, &uart_cfg), TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(UART_NUM_1, WDAP_BACKEND_UART_TX_GPIO, WDAP_BACKEND_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart_set_pin failed");

    backend_swd_gpio_port_swd_setup();
    ESP_LOGI(TAG, "SWD init ok: SWCLK=GPIO%d SWDIO=GPIO%d nRESET=GPIO%d UART_TX=GPIO%d UART_RX=GPIO%d baud=%d",
             WDAP_BACKEND_SWCLK_GPIO, WDAP_BACKEND_SWDIO_GPIO, WDAP_BACKEND_NRESET_GPIO,
             WDAP_BACKEND_UART_TX_GPIO, WDAP_BACKEND_UART_RX_GPIO, WDAP_BACKEND_UART_BAUDRATE);
    return ESP_OK;
}

void backend_swd_gpio_port_swd_setup(void)
{
    gpio_set_direction(WDAP_BACKEND_SWCLK_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(WDAP_BACKEND_SWCLK_GPIO, 1);
    backend_swd_gpio_swdio_output_enable();
    gpio_set_level(WDAP_BACKEND_SWDIO_GPIO, 1);
    backend_swd_gpio_nreset_write(1);
}

void backend_swd_gpio_port_off(void)
{
    gpio_set_level(WDAP_BACKEND_SWCLK_GPIO, 0);
    backend_swd_gpio_swdio_output_disable();
    backend_swd_gpio_nreset_write(1);
}

void backend_swd_gpio_swclk_write(uint32_t bit)
{
    gpio_set_level(WDAP_BACKEND_SWCLK_GPIO, bit ? 1 : 0);
}

uint32_t backend_swd_gpio_swclk_read(void)
{
    return (uint32_t)gpio_get_level(WDAP_BACKEND_SWCLK_GPIO);
}

void backend_swd_gpio_swdio_write(uint32_t bit)
{
    gpio_set_level(WDAP_BACKEND_SWDIO_GPIO, bit ? 1 : 0);
}

uint32_t backend_swd_gpio_swdio_read(void)
{
    return (uint32_t)gpio_get_level(WDAP_BACKEND_SWDIO_GPIO);
}

void backend_swd_gpio_swdio_output_enable(void)
{
    gpio_set_direction(WDAP_BACKEND_SWDIO_GPIO, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_pull_mode(WDAP_BACKEND_SWDIO_GPIO, GPIO_PULLUP_ONLY);
}

void backend_swd_gpio_swdio_output_disable(void)
{
    gpio_set_direction(WDAP_BACKEND_SWDIO_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(WDAP_BACKEND_SWDIO_GPIO, GPIO_PULLUP_ONLY);
}

void backend_swd_gpio_nreset_write(uint32_t bit)
{
    if (bit != 0) {
        gpio_set_level(WDAP_BACKEND_NRESET_GPIO, 1);
    } else {
        gpio_set_level(WDAP_BACKEND_NRESET_GPIO, 0);
    }
}

uint32_t backend_swd_gpio_nreset_read(void)
{
    return (uint32_t)gpio_get_level(WDAP_BACKEND_NRESET_GPIO);
}
