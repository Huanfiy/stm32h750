#include "app_drv_adc.h"
#include "app_drv_gpio.h"

#include <board.h>
#include <rtdevice.h>
#include <string.h>

#define ADC1_CH_COUNT       14U
/* PC3_C, PC2_C + VREFINT self-check (needs >= 4.3us sampling, so it doubles
 * as a closed-loop guard against the sampling time regressing). */
#define ADC3_CH_COUNT       3U
#define ADC_VREF_MV         3300U
#define ADC_MAX_RAW         65535U
#define INA240A2_GAIN       50U
#define SHUNT_RESISTOR_MOHM 50U
/* Set to the measured INA240 REF voltage when REF is biased above GND. */
#define INA240_REF_MV       0U

#define ADC_IRQ_PRIORITY    5U

/* MSP (board/CubeMX_Config/Core/Src/stm32h7xx_hal_msp.c) does HAL_DMA_Init
 * + __HAL_LINKDMA against these names — must be globally visible. */
ADC_HandleTypeDef hadc1;
ADC_HandleTypeDef hadc3;
extern DMA_HandleTypeDef hdma_adc1;  /* defined in board/board.c */
extern DMA_HandleTypeDef hdma_adc3;

static TIM_HandleTypeDef htim6;

static uint16_t adc1_dma_buf[ADC1_CH_COUNT] __attribute__((section(".dma_buffer"), aligned(32)));
static uint16_t adc3_dma_buf[ADC3_CH_COUNT] __attribute__((section(".ram_d3"), aligned(32)));
static uint16_t adc_snapshot[APP_DRV_ADC_TOTAL_CH];
static uint16_t adc_vrefint_raw;
static struct rt_semaphore adc_sem;
static rt_bool_t adc_inited;

static rt_err_t adc1_configure(void)
{
    static const uint32_t channels[ADC1_CH_COUNT] = {
        ADC_CHANNEL_3, ADC_CHANNEL_4, ADC_CHANNEL_5, ADC_CHANNEL_7,
        ADC_CHANNEL_8, ADC_CHANNEL_9, ADC_CHANNEL_10, ADC_CHANNEL_11,
        ADC_CHANNEL_14, ADC_CHANNEL_15, ADC_CHANNEL_16, ADC_CHANNEL_17,
        ADC_CHANNEL_18, ADC_CHANNEL_19,
    };
    static const uint32_t ranks[ADC1_CH_COUNT] = {
        ADC_REGULAR_RANK_1,  ADC_REGULAR_RANK_2,  ADC_REGULAR_RANK_3,  ADC_REGULAR_RANK_4,
        ADC_REGULAR_RANK_5,  ADC_REGULAR_RANK_6,  ADC_REGULAR_RANK_7,  ADC_REGULAR_RANK_8,
        ADC_REGULAR_RANK_9,  ADC_REGULAR_RANK_10, ADC_REGULAR_RANK_11, ADC_REGULAR_RANK_12,
        ADC_REGULAR_RANK_13, ADC_REGULAR_RANK_14,
    };

    ADC_MultiModeTypeDef multimode = {0};
    ADC_ChannelConfTypeDef sConfig = {0};

    hadc1.Instance = ADC1;
    hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV2;
    hadc1.Init.Resolution = ADC_RESOLUTION_16B;
    hadc1.Init.ScanConvMode = ADC_SCAN_ENABLE;
    hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    hadc1.Init.LowPowerAutoWait = DISABLE;
    hadc1.Init.ContinuousConvMode = DISABLE;
    hadc1.Init.NbrOfConversion = ADC1_CH_COUNT;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIG_T6_TRGO;
    hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
    hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DMA_CIRCULAR;
    hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
    hadc1.Init.LeftBitShift = ADC_LEFTBITSHIFT_NONE;
    hadc1.Init.OversamplingMode = DISABLE;
    if (HAL_ADC_Init(&hadc1) != HAL_OK) return -RT_ERROR;

    multimode.Mode = ADC_MODE_INDEPENDENT;
    if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) != HAL_OK) return -RT_ERROR;

    /* 100Hz scan leaves a huge budget (14ch x 819cyc @ 37.5MHz = 306us per
     * frame), so use the max sampling time: the INA240 outputs go through
     * per-channel RC networks that do not settle in 1.5 cycles (40ns). */
    sConfig.SamplingTime = ADC_SAMPLETIME_810CYCLES_5;
    sConfig.SingleDiff = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber = ADC_OFFSET_NONE;
    sConfig.Offset = 0;
    sConfig.OffsetSignedSaturation = DISABLE;
    for (uint32_t i = 0; i < ADC1_CH_COUNT; i++) {
        sConfig.Channel = channels[i];
        sConfig.Rank = ranks[i];
        if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) return -RT_ERROR;
    }
    return RT_EOK;
}

static rt_err_t adc3_configure(void)
{
    ADC_ChannelConfTypeDef sConfig = {0};

    hadc3.Instance = ADC3;
    hadc3.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV2;
    hadc3.Init.Resolution = ADC_RESOLUTION_16B;
    hadc3.Init.ScanConvMode = ADC_SCAN_ENABLE;
    hadc3.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    hadc3.Init.LowPowerAutoWait = DISABLE;
    hadc3.Init.ContinuousConvMode = DISABLE;
    hadc3.Init.NbrOfConversion = ADC3_CH_COUNT;
    hadc3.Init.DiscontinuousConvMode = DISABLE;
    hadc3.Init.ExternalTrigConv = ADC_EXTERNALTRIG_T6_TRGO;
    hadc3.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
    hadc3.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DMA_CIRCULAR;
    hadc3.Init.Overrun = ADC_OVR_DATA_PRESERVED;
    hadc3.Init.LeftBitShift = ADC_LEFTBITSHIFT_NONE;
    hadc3.Init.OversamplingMode = DISABLE;
    if (HAL_ADC_Init(&hadc3) != HAL_OK) return -RT_ERROR;

    sConfig.SamplingTime = ADC_SAMPLETIME_810CYCLES_5;
    sConfig.SingleDiff = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber = ADC_OFFSET_NONE;
    sConfig.Offset = 0;
    sConfig.OffsetSignedSaturation = DISABLE;

    sConfig.Channel = ADC_CHANNEL_1;       /* PC3 → snapshot[14] */
    sConfig.Rank = ADC_REGULAR_RANK_1;
    if (HAL_ADC_ConfigChannel(&hadc3, &sConfig) != HAL_OK) return -RT_ERROR;

    sConfig.Channel = ADC_CHANNEL_0;       /* PC2 → snapshot[15] */
    sConfig.Rank = ADC_REGULAR_RANK_2;
    if (HAL_ADC_ConfigChannel(&hadc3, &sConfig) != HAL_OK) return -RT_ERROR;

    sConfig.Channel = ADC_CHANNEL_VREFINT; /* ~1.21V internal reference */
    sConfig.Rank = ADC_REGULAR_RANK_3;
    if (HAL_ADC_ConfigChannel(&hadc3, &sConfig) != HAL_OK) return -RT_ERROR;

    return RT_EOK;
}

static rt_err_t tim6_configure(void)
{
    TIM_MasterConfigTypeDef sMasterConfig = {0};

    __HAL_RCC_TIM6_CLK_ENABLE();

    htim6.Instance = TIM6;
    htim6.Init.Prescaler = 23999;            /* 240MHz / 24000 = 10kHz tick */
    htim6.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim6.Init.Period = 99;                  /* 10kHz / 100 = 100Hz scan rate */
    htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim6) != HAL_OK) return -RT_ERROR;

    sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
    sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim6, &sMasterConfig) != HAL_OK) return -RT_ERROR;

    return RT_EOK;
}

static void invalidate_dcache(void *addr, int32_t size)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) {
        SCB_InvalidateDCache_by_Addr((uint32_t *)addr, size);
    }
#else
    (void)addr; (void)size;
#endif
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC3) {
        invalidate_dcache(adc3_dma_buf, sizeof(adc3_dma_buf));
        memcpy(&adc_snapshot[14], adc3_dma_buf, 2U * sizeof(adc3_dma_buf[0]));
        adc_vrefint_raw = adc3_dma_buf[2];
    } else if (hadc->Instance == ADC1) {
        /* ADC1 has 14ch vs ADC3's 2ch at the same clock — ADC1 always finishes
         * later within one TIM6 trigger, so we use its completion as the
         * "frame ready" boundary. */
        invalidate_dcache(adc1_dma_buf, sizeof(adc1_dma_buf));
        memcpy(adc_snapshot, adc1_dma_buf, sizeof(adc1_dma_buf));
        rt_sem_release(&adc_sem);
    }
}

void DMA1_Stream0_IRQHandler(void)
{
    rt_interrupt_enter();
    HAL_DMA_IRQHandler(&hdma_adc1);
    rt_interrupt_leave();
}

void BDMA_Channel0_IRQHandler(void)
{
    rt_interrupt_enter();
    HAL_DMA_IRQHandler(&hdma_adc3);
    rt_interrupt_leave();
}

int app_drv_adc_init(void)
{
    if (adc_inited) return RT_EOK;

    rt_sem_init(&adc_sem, "adc_sem", 0, RT_IPC_FLAG_PRIO);

    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_BDMA_CLK_ENABLE();

    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, ADC_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
    HAL_NVIC_SetPriority(BDMA_Channel0_IRQn, ADC_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(BDMA_Channel0_IRQn);

    if (adc1_configure() != RT_EOK) return -RT_ERROR;
    if (adc3_configure() != RT_EOK) return -RT_ERROR;
    if (tim6_configure() != RT_EOK) return -RT_ERROR;

    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK) return -RT_ERROR;
    if (HAL_ADCEx_Calibration_Start(&hadc3, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK) return -RT_ERROR;

    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc1_dma_buf, ADC1_CH_COUNT) != HAL_OK) return -RT_ERROR;
    if (HAL_ADC_Start_DMA(&hadc3, (uint32_t *)adc3_dma_buf, ADC3_CH_COUNT) != HAL_OK) return -RT_ERROR;

    if (HAL_TIM_Base_Start(&htim6) != HAL_OK) return -RT_ERROR;

    adc_inited = RT_TRUE;
    return RT_EOK;
}
INIT_DEVICE_EXPORT(app_drv_adc_init);

int app_drv_adc_wait(rt_int32_t timeout_ms)
{
    rt_tick_t ticks = (timeout_ms < 0) ? RT_WAITING_FOREVER : rt_tick_from_millisecond(timeout_ms);
    return rt_sem_take(&adc_sem, ticks);
}

void app_drv_adc_get_snapshot(uint16_t out[APP_DRV_ADC_TOTAL_CH])
{
    /* The writer is the ADC DMA-complete ISR — scheduler locks don't stop
     * it, so mask interrupts for the 32-byte copy. */
    rt_base_t level = rt_hw_interrupt_disable();
    memcpy(out, adc_snapshot, sizeof(adc_snapshot));
    rt_hw_interrupt_enable(level);
}

uint32_t app_drv_adc_raw_to_mv(uint16_t raw)
{
    return ((uint32_t)raw * ADC_VREF_MV) / ADC_MAX_RAW;
}

uint16_t app_drv_adc_get_vrefint_raw(void)
{
    return adc_vrefint_raw;
}

uint32_t app_drv_adc_raw_to_current_ma(uint16_t raw)
{
    uint32_t mv = app_drv_adc_raw_to_mv(raw);

    if (mv <= INA240_REF_MV) {
        return 0;
    }
    return ((mv - INA240_REF_MV) * 1000U) / (INA240A2_GAIN * SHUNT_RESISTOR_MOHM);
}

#ifdef RT_USING_FINSH
#include <finsh.h>

static int cmd_adc_dump(int argc, char **argv)
{
    static const char * const pin_names[APP_DRV_ADC_TOTAL_CH] = {
        "PA6", "PC4", "PB1", "PA7", "PC5", "PB0", "PC0", "PC1",
        "PA2", "PA3", "PA0", "PA1", "PA4", "PA5", "PC3", "PC2",
    };
    static const app_drv_gpio_ch_t pwr_channels[APP_DRV_ADC_TOTAL_CH] = {
        PWR_EN1,  PWR_EN2,  PWR_EN3,  PWR_EN4,
        PWR_EN5,  PWR_EN6,  PWR_EN7,  PWR_EN8,
        PWR_EN9,  PWR_EN10, PWR_EN11, PWR_EN12,
        PWR_EN13, PWR_EN14, PWR_EN15, PWR_EN16,
    };
    if (app_drv_adc_wait(1000) != RT_EOK) {
        rt_kprintf("adc: timeout waiting for snapshot\n");
        return -1;
    }
    uint16_t s[APP_DRV_ADC_TOTAL_CH];
    app_drv_adc_get_snapshot(s);
    for (uint32_t i = 0; i < APP_DRV_ADC_TOTAL_CH; i++) {
        int pwr = app_drv_gpio.read(pwr_channels[i]);
        const char *pwr_state = (pwr < 0) ? "-" : (pwr ? "1" : "0");

        rt_kprintf("pwr=%s  ch%02u %s: raw=%5u  mA=%4u\n",
                   pwr_state, i, pin_names[i], s[i], app_drv_adc_raw_to_current_ma(s[i]));
    }
    uint16_t vref = app_drv_adc_get_vrefint_raw();
    rt_kprintf("vrefint: raw=%5u  mV=%4u\n", vref, app_drv_adc_raw_to_mv(vref));
    (void)argc; (void)argv;
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_adc_dump, adc_dump, dump latest 16-ch ADC snapshot);
#endif
