#pragma once

#include <stdint.h>
#include "board_config.h"
#include "backend_swd_gpio.h"
#include "esp_timer.h"
#include "soc/gpio_struct.h"

#define CPU_CLOCK               240000000U
#define IO_PORT_WRITE_CYCLES    24U

#define DAP_SWD                 1
#define DAP_JTAG                0
#define DAP_JTAG_DEV_CNT        0
#define DAP_DEFAULT_PORT        1
#define DAP_DEFAULT_SWJ_CLOCK   WDAP_DEFAULT_SWJ_CLOCK_HZ
#define DAP_PACKET_SIZE         WDAP_DAP_PACKET_SIZE
#define DAP_PACKET_COUNT        WDAP_DAP_PACKET_COUNT

#define SWO_UART                0
#define SWO_UART_DRIVER         0
#define SWO_UART_MAX_BAUDRATE   0U
#define SWO_MANCHESTER          0
#define SWO_BUFFER_SIZE         0U
#define SWO_STREAM              0
#define TIMESTAMP_CLOCK         1000000U
#define DAP_UART                0
#define DAP_UART_DRIVER         0
#define DAP_UART_RX_BUFFER_SIZE 0U
#define DAP_UART_TX_BUFFER_SIZE 0U
#define DAP_UART_USB_COM_PORT   0
#define TARGET_FIXED            0

#define CMSIS_DAP_VENDOR_NAME   WDAP_USB_MANUFACTURER
#define CMSIS_DAP_PRODUCT_NAME  WDAP_USB_PRODUCT

#define WDAP_SWCLK_MASK         (1U << WDAP_BACKEND_SWCLK_GPIO)
#define WDAP_SWDIO_MASK         (1U << WDAP_BACKEND_SWDIO_GPIO)
#define WDAP_NRESET_MASK        (1U << WDAP_BACKEND_NRESET_GPIO)

#define PIN_SWCLK_TCK_SET()     do { GPIO.out_w1ts = WDAP_SWCLK_MASK; } while (0)
#define PIN_SWCLK_TCK_CLR()     do { GPIO.out_w1tc = WDAP_SWCLK_MASK; } while (0)
#define PIN_SWCLK_TCK_IN()      ((GPIO.in >> WDAP_BACKEND_SWCLK_GPIO) & 1U)
#define PIN_SWDIO_TMS_SET()     do { GPIO.out_w1ts = WDAP_SWDIO_MASK; } while (0)
#define PIN_SWDIO_TMS_CLR()     do { GPIO.out_w1tc = WDAP_SWDIO_MASK; } while (0)
#define PIN_SWDIO_TMS_IN()      ((GPIO.in >> WDAP_BACKEND_SWDIO_GPIO) & 1U)
#define PIN_SWDIO_OUT(bit)      do { if ((bit) & 1U) { GPIO.out_w1ts = WDAP_SWDIO_MASK; } else { GPIO.out_w1tc = WDAP_SWDIO_MASK; } } while (0)
#define PIN_SWDIO_IN()          ((GPIO.in >> WDAP_BACKEND_SWDIO_GPIO) & 1U)
#define PIN_SWDIO_OUT_ENABLE()  do { GPIO.enable_w1ts = WDAP_SWDIO_MASK; } while (0)
#define PIN_SWDIO_OUT_DISABLE() do { GPIO.enable_w1tc = WDAP_SWDIO_MASK; } while (0)

#define PIN_TDI_OUT(bit)        do { (void)(bit); } while (0)
#define PIN_TDI_IN()            (1U)
#define PIN_TDO_IN()            (1U)
#define PIN_nTRST_OUT(bit)      do { (void)(bit); } while (0)
#define PIN_nTRST_IN()          (1U)
#define PIN_nRESET_OUT(bit)     do { if ((bit) & 1U) { GPIO.out_w1ts = WDAP_NRESET_MASK; } else { GPIO.out_w1tc = WDAP_NRESET_MASK; } } while (0)
#define PIN_nRESET_IN()         ((GPIO.in >> WDAP_BACKEND_NRESET_GPIO) & 1U)

#define PORT_SWD_SETUP()        backend_swd_gpio_port_swd_setup()
#define PORT_JTAG_SETUP()       do { } while (0)
#define PORT_OFF()              backend_swd_gpio_port_off()

#define LED_CONNECTED_OUT(bit)  do { (void)(bit); } while (0)
#define LED_RUNNING_OUT(bit)    do { (void)(bit); } while (0)

static inline uint32_t TIMESTAMP_GET(void)
{
    return (uint32_t)esp_timer_get_time();
}

static inline void DAP_SETUP(void)
{
    backend_swd_gpio_init();
}

static inline uint32_t RESET_TARGET(void)
{
    return 0U;
}
