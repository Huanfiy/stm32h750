#include "boot_qspi.h"

QSPI_HandleTypeDef hqspi;

void HAL_QSPI_MspInit(QSPI_HandleTypeDef *qspi)
{
    GPIO_InitTypeDef gpio = {0};
    RCC_PeriphCLKInitTypeDef periph = {0};

    if (qspi->Instance != QUADSPI)
    {
        return;
    }

    /* QSPI kernel clock = D1HCLK (matches CubeMX project). */
    periph.PeriphClockSelection = RCC_PERIPHCLK_QSPI;
    periph.QspiClockSelection   = RCC_QSPICLKSOURCE_D1HCLK;
    HAL_RCCEx_PeriphCLKConfig(&periph);

    __HAL_RCC_QSPI_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    /* CLK = PB2  AF9 */
    /* IO2 = PE2  AF9 */
    /* IO0 = PD11 AF9 */
    /* IO1 = PD12 AF9 */
    /* IO3 = PD13 AF9 */
    /* NCS = PB6  AF10 */

    gpio.Mode  = GPIO_MODE_AF_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

    gpio.Pin       = GPIO_PIN_2;
    gpio.Alternate = GPIO_AF9_QUADSPI;
    HAL_GPIO_Init(GPIOB, &gpio);

    gpio.Pin       = GPIO_PIN_2;
    gpio.Alternate = GPIO_AF9_QUADSPI;
    HAL_GPIO_Init(GPIOE, &gpio);

    gpio.Pin       = GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13;
    gpio.Alternate = GPIO_AF9_QUADSPI;
    HAL_GPIO_Init(GPIOD, &gpio);

    gpio.Pin       = GPIO_PIN_6;
    gpio.Alternate = GPIO_AF10_QUADSPI;
    HAL_GPIO_Init(GPIOB, &gpio);
}

int boot_qspi_init(void)
{
    hqspi.Instance                = QUADSPI;
    hqspi.Init.ClockPrescaler     = 1;                          /* QSPI clk = D1HCLK / 2 */
    hqspi.Init.FifoThreshold      = 4;
    hqspi.Init.SampleShifting     = QSPI_SAMPLE_SHIFTING_HALFCYCLE;
    hqspi.Init.FlashSize           = 22;                        /* 2^(22+1) = 8 MB */
    hqspi.Init.ChipSelectHighTime = QSPI_CS_HIGH_TIME_4_CYCLE;
    hqspi.Init.ClockMode          = QSPI_CLOCK_MODE_0;
    hqspi.Init.FlashID            = QSPI_FLASH_ID_1;
    hqspi.Init.DualFlash          = QSPI_DUALFLASH_DISABLE;

    if (HAL_QSPI_Init(&hqspi) != HAL_OK)
    {
        return -1;
    }
    return 0;
}
