#include "boot_uart.h"
#include "boot_config.h"
#include "stm32h7xx_hal.h"
#include <stdarg.h>
#include <stdio.h>

static UART_HandleTypeDef huart_dbg;

void HAL_UART_MspInit(UART_HandleTypeDef *uartHandle)
{
    GPIO_InitTypeDef gpio = {0};
    RCC_PeriphCLKInitTypeDef periph = {0};

    if (uartHandle->Instance != USART1)
    {
        return;
    }

    periph.PeriphClockSelection = RCC_PERIPHCLK_USART1;
    periph.Usart16ClockSelection = RCC_USART16CLKSOURCE_D2PCLK2;
    HAL_RCCEx_PeriphCLKConfig(&periph);

    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* PB14 = USART1_TX, PB15 = USART1_RX, AF4 */
    gpio.Pin       = GPIO_PIN_14 | GPIO_PIN_15;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF4_USART1;
    HAL_GPIO_Init(GPIOB, &gpio);
}

void boot_uart_init(void)
{
    huart_dbg.Instance                    = BOOT_UART_INSTANCE;
    huart_dbg.Init.BaudRate               = BOOT_UART_BAUDRATE;
    huart_dbg.Init.WordLength             = UART_WORDLENGTH_8B;
    huart_dbg.Init.StopBits               = UART_STOPBITS_1;
    huart_dbg.Init.Parity                 = UART_PARITY_NONE;
    huart_dbg.Init.Mode                   = UART_MODE_TX_RX;
    huart_dbg.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    huart_dbg.Init.OverSampling           = UART_OVERSAMPLING_16;
    huart_dbg.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    huart_dbg.Init.ClockPrescaler         = UART_PRESCALER_DIV1;
    huart_dbg.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

    if (HAL_UART_Init(&huart_dbg) != HAL_OK)
    {
        /* Stuck in a tight loop; debugger can still attach. */
        while (1) { }
    }
}

void boot_uart_write(const char *buf, uint32_t len)
{
    HAL_UART_Transmit(&huart_dbg, (uint8_t *)buf, len, HAL_MAX_DELAY);
}

void boot_log(const char *fmt, ...)
{
    static char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        return;
    }
    if ((uint32_t)n > sizeof(line) - 1)
    {
        n = sizeof(line) - 1;
    }
    boot_uart_write(line, (uint32_t)n);
}
