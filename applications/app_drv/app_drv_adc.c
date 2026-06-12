#include "app_drv_adc.h"
#include "app_drv_gpio.h"

#include <board.h>
#include <rtdevice.h>
#include <stdlib.h>
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

#define ADC_ZERO_BOOT_FRAMES 50U    /* 500 ms baseline at power-up */
#define ADC_ZERO_MAX_FRAMES  1000U

static uint16_t adc_zero_raw[APP_DRV_ADC_TOTAL_CH];
static uint32_t adc_zero_frames;    /* 0 = never calibrated */

int app_drv_adc_zero_calibrate(uint32_t frames)
{
    uint32_t acc[APP_DRV_ADC_TOTAL_CH] = {0};
    uint16_t table[APP_DRV_ADC_TOTAL_CH];

    if (!adc_inited) return -RT_ERROR;
    if (frames < 1U || frames > ADC_ZERO_MAX_FRAMES) return -RT_EINVAL;

    rt_sem_control(&adc_sem, RT_IPC_CMD_RESET, (void *)0);
    for (uint32_t f = 0; f < frames; f++) {
        uint16_t s[APP_DRV_ADC_TOTAL_CH];

        if (app_drv_adc_wait(100) != RT_EOK) return -RT_ETIMEOUT;
        app_drv_adc_get_snapshot(s);
        for (uint32_t i = 0; i < APP_DRV_ADC_TOTAL_CH; i++) {
            acc[i] += s[i];
        }
    }
    for (uint32_t i = 0; i < APP_DRV_ADC_TOTAL_CH; i++) {
        table[i] = (uint16_t)(acc[i] / frames);
    }

    /* Readers (adc_dump / monitor thread) index single uint16 entries; swap
     * the whole table atomically so no reader sees a half-updated baseline. */
    rt_base_t level = rt_hw_interrupt_disable();
    memcpy(adc_zero_raw, table, sizeof(adc_zero_raw));
    adc_zero_frames = frames;
    rt_hw_interrupt_enable(level);
    return RT_EOK;
}

void app_drv_adc_get_zero(uint16_t out[APP_DRV_ADC_TOTAL_CH])
{
    rt_base_t level = rt_hw_interrupt_disable();
    memcpy(out, adc_zero_raw, sizeof(adc_zero_raw));
    rt_hw_interrupt_enable(level);
}

uint32_t app_drv_adc_corrected_ma(uint32_t idx, uint16_t raw)
{
    uint16_t zero = adc_zero_raw[idx % APP_DRV_ADC_TOTAL_CH];

    if (raw <= zero) {
        return 0;
    }
    return app_drv_adc_raw_to_current_ma((uint16_t)(raw - zero));
}

static int app_drv_adc_zero_boot(void)
{
    /* INIT_APP runs after every INIT_DEVICE hook, so all PWR_EN lines are
     * already latched inactive — this captures the true zero-current
     * baseline before any channel can be powered. */
    int err = app_drv_adc_zero_calibrate(ADC_ZERO_BOOT_FRAMES);

    if (err != RT_EOK) {
        rt_kprintf("[adc] zero calibration failed (%d)\n", err);
    } else {
        rt_kprintf("[adc] zero calibration done (%u frames)\n", ADC_ZERO_BOOT_FRAMES);
    }
    return 0;   /* never block the rest of boot */
}
INIT_APP_EXPORT(app_drv_adc_zero_boot);

#ifdef RT_USING_FINSH
#include <finsh.h>

typedef struct {
    uint8_t chn;
    app_drv_gpio_ch_t pwr_ch;
    const char *pwr_pin;
    uint8_t adc_idx;
    const char *adc_ch;
    const char *adc_pin;
} adc_dump_row_t;

static const adc_dump_row_t adc_rows[APP_DRV_ADC_TOTAL_CH] = {
    {1U,  PWR_EN1,  "PE1",  0U,  "ADC1_INP3",  "PA6"},
    {2U,  PWR_EN2,  "PB9",  1U,  "ADC1_INP4",  "PC4"},
    {3U,  PWR_EN3,  "PB7",  2U,  "ADC1_INP5",  "PB1"},
    {4U,  PWR_EN4,  "PB5",  3U,  "ADC1_INP7",  "PA7"},
    {5U,  PWR_EN5,  "PB3",  4U,  "ADC1_INP8",  "PC5"},
    {6U,  PWR_EN6,  "PD6",  5U,  "ADC1_INP9",  "PB0"},
    {7U,  PWR_EN7,  "PD2",  6U,  "ADC1_INP10", "PC0"},
    {8U,  PWR_EN8,  "PD4",  7U,  "ADC1_INP11", "PC1"},
    {9U,  PWR_EN9,  "PE7",  8U,  "ADC1_INP14", "PA2"},
    {10U, PWR_EN10, "PE9",  9U,  "ADC1_INP15", "PA3"},
    {11U, PWR_EN11, "PE13", 10U, "ADC1_INP16", "PA0"},
    {12U, PWR_EN12, "PE11", 11U, "ADC1_INP17", "PA1"},
    {13U, PWR_EN13, "PE12", 12U, "ADC1_INP18", "PA4"},
    {14U, PWR_EN14, "PE10", 13U, "ADC1_INP19", "PA5"},
    {15U, PWR_EN15, "PB2",  15U, "ADC3_INP0",  "PC2"},
    {16U, PWR_EN16, "PE8",  14U, "ADC3_INP1",  "PC3"},
};

static int cmd_adc_dump(int argc, char **argv)
{
    const adc_dump_row_t *rows = adc_rows;
    if (app_drv_adc_wait(1000) != RT_EOK) {
        rt_kprintf("adc: timeout waiting for snapshot\n");
        return -1;
    }
    uint16_t s[APP_DRV_ADC_TOTAL_CH];
    uint16_t zero[APP_DRV_ADC_TOTAL_CH];
    app_drv_adc_get_snapshot(s);
    app_drv_adc_get_zero(zero);
    for (uint32_t i = 0; i < APP_DRV_ADC_TOTAL_CH; i++) {
        const adc_dump_row_t *row = &rows[i];
        int pwr = app_drv_gpio.read(row->pwr_ch);
        const char *pwr_state = (pwr < 0) ? "-" : (pwr ? "1" : "0");
        uint16_t raw = s[row->adc_idx];

        if (i == 0U) {
            rt_kprintf("chn pwr_en en_pin adc_ch      adc_pin raw   zero_ma ma\n");
        }
        rt_kprintf("%-3u %-6s %-6s %-11s %-7s %5u %7u %5u\n",
                   row->chn, pwr_state, row->pwr_pin, row->adc_ch, row->adc_pin,
                   raw, app_drv_adc_raw_to_current_ma(zero[row->adc_idx]),
                   app_drv_adc_corrected_ma(row->adc_idx, raw));
    }
    uint16_t vref = app_drv_adc_get_vrefint_raw();
    rt_kprintf("vrefint: raw=%5u  mV=%4u\n", vref, app_drv_adc_raw_to_mv(vref));
    (void)argc; (void)argv;
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_adc_dump, adc_dump, dump latest 16-ch ADC snapshot);

static int cmd_adc_zero(int argc, char **argv)
{
    if (argc > 1) {
        uint32_t frames = (uint32_t)atoi(argv[1]);
        int err = app_drv_adc_zero_calibrate(frames);

        if (err != RT_EOK) {
            rt_kprintf("adc_zero: calibration failed (%d), frames must be 1..%u\n",
                       err, ADC_ZERO_MAX_FRAMES);
            return -1;
        }
        rt_kprintf("adc_zero: ok frames=%u\n", frames);
        return 0;
    }

    /* Status: per-channel baseline in raw counts, chn display order. */
    uint16_t zero[APP_DRV_ADC_TOTAL_CH];
    app_drv_adc_get_zero(zero);
    rt_kprintf("adc_zero: frames=%u%s\n", adc_zero_frames,
               (adc_zero_frames == 0U) ? " (not calibrated)" : "");
    rt_kprintf("zero_raw:");
    for (uint32_t i = 0; i < APP_DRV_ADC_TOTAL_CH; i++) {
        rt_kprintf(" %5u", zero[adc_rows[i].adc_idx]);
    }
    rt_kprintf("\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_adc_zero, adc_zero, show or redo ADC zero-offset calibration);

/* Slot layout for the stats/trace commands: 0..15 = snapshot indices
 * (adc_rows[].adc_idx), 16 = VREFINT. VREFINT never leaves the die, so its
 * noise floor isolates "ADC config/core problem" from "board/input problem". */
#define ADC_STAT_SLOTS      (APP_DRV_ADC_TOTAL_CH + 1U)
#define ADC_STAT_VREF_SLOT  APP_DRV_ADC_TOTAL_CH
#define ADC_STAT_MAX_FRAMES 10000U  /* keeps sum^2 and sumsq*N inside u64 */

static uint32_t isqrt64(uint64_t v)
{
    uint64_t res = 0U;
    uint64_t bit = 1ULL << 62;

    while (bit > v) bit >>= 2;
    while (bit != 0U) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)res;
}

static void adc_frame_read(uint16_t vals[ADC_STAT_SLOTS])
{
    app_drv_adc_get_snapshot(vals);
    vals[ADC_STAT_VREF_SLOT] = app_drv_adc_get_vrefint_raw();
}

static int cmd_adc_stat(int argc, char **argv)
{
    static uint16_t s_min[ADC_STAT_SLOTS];
    static uint16_t s_max[ADC_STAT_SLOTS];
    static uint64_t s_sum[ADC_STAT_SLOTS];
    static uint64_t s_sumsq[ADC_STAT_SLOTS];
    uint32_t frames = 500U;

    if (argc > 1) {
        frames = (uint32_t)atoi(argv[1]);
        if (frames < 10U || frames > ADC_STAT_MAX_FRAMES) {
            rt_kprintf("usage: adc_stat [frames]  (10..%u, default 500; 100Hz frames)\n",
                       ADC_STAT_MAX_FRAMES);
            return -1;
        }
    }

    for (uint32_t i = 0; i < ADC_STAT_SLOTS; i++) {
        s_min[i] = 0xFFFFU;
        s_max[i] = 0U;
        s_sum[i] = 0U;
        s_sumsq[i] = 0U;
    }

    /* The ISR releases the sem at 100Hz with no consumer between commands, so
     * the count has piled up — reset it so every take below is a fresh frame. */
    rt_sem_control(&adc_sem, RT_IPC_CMD_RESET, (void *)0);
    rt_tick_t t0 = rt_tick_get();
    for (uint32_t f = 0; f < frames; f++) {
        uint16_t vals[ADC_STAT_SLOTS];

        if (app_drv_adc_wait(100) != RT_EOK) {
            rt_kprintf("adc_stat: timeout at frame %u\n", f);
            return -1;
        }
        adc_frame_read(vals);
        for (uint32_t i = 0; i < ADC_STAT_SLOTS; i++) {
            uint16_t v = vals[i];
            if (v < s_min[i]) s_min[i] = v;
            if (v > s_max[i]) s_max[i] = v;
            s_sum[i] += v;
            s_sumsq[i] += (uint64_t)v * v;
        }
    }
    uint32_t elapsed_ms = (uint32_t)((rt_tick_get() - t0) * 1000U / RT_TICK_PER_SECOND);

    rt_kprintf("adc_stat: frames=%u elapsed=%u ms (nominal 10 ms/frame)\n", frames, elapsed_ms);
    rt_kprintf("chn adc_ch      pin     pwr   mean    min    max   p2p    std     mV\n");
    for (uint32_t i = 0; i <= APP_DRV_ADC_TOTAL_CH; i++) {
        uint32_t slot, mean, std10, mv;
        uint64_t var100;
        const char *name, *pin, *pwr = "-";

        if (i < APP_DRV_ADC_TOTAL_CH) {
            const adc_dump_row_t *row = &adc_rows[i];
            int p = app_drv_gpio.read(row->pwr_ch);
            slot = row->adc_idx;
            name = row->adc_ch;
            pin = row->adc_pin;
            pwr = (p < 0) ? "-" : (p ? "1" : "0");
        } else {
            slot = ADC_STAT_VREF_SLOT;
            name = "vrefint";
            pin = "-";
        }
        mean = (uint32_t)(s_sum[slot] / frames);
        /* var*100 = (N*sumsq - sum^2) * 100 / N^2; isqrt gives std*10. */
        var100 = ((uint64_t)frames * s_sumsq[slot] - s_sum[slot] * s_sum[slot]) * 100U
                 / ((uint64_t)frames * frames);
        std10 = isqrt64(var100);
        mv = app_drv_adc_raw_to_mv((uint16_t)mean);
        rt_kprintf("%-3u %-11s %-7s %-3s %6u %6u %6u %5u %4u.%u %6u\n",
                   i + 1U, name, pin, pwr, mean, s_min[slot], s_max[slot],
                   (uint32_t)(s_max[slot] - s_min[slot]), std10 / 10U, std10 % 10U, mv);
    }
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_adc_stat, adc_stat, per-channel min/max/mean/std over N 100Hz frames);

static int cmd_adc_trace(int argc, char **argv)
{
    /* adc_trace <frames> <ch>...  — ch: 1..16 or 'v' (vrefint), up to 6 cols.
     * One CSV row per 100Hz frame; ~50 chars/row stays well under the 10 ms
     * frame budget at 115200 baud. */
    uint32_t slots[6];
    uint32_t ncols = (uint32_t)argc - 2U;
    uint32_t frames;

    if (argc < 3 || ncols > 6U) {
        rt_kprintf("usage: adc_trace <frames 1..%u> <ch>...  (ch: 1..16 or v, max 6)\n",
                   ADC_STAT_MAX_FRAMES);
        return -1;
    }
    frames = (uint32_t)atoi(argv[1]);
    if (frames < 1U || frames > ADC_STAT_MAX_FRAMES) {
        rt_kprintf("adc_trace: bad frame count '%s'\n", argv[1]);
        return -1;
    }
    for (uint32_t c = 0; c < ncols; c++) {
        const char *a = argv[2 + c];
        if (a[0] == 'v' && a[1] == '\0') {
            slots[c] = ADC_STAT_VREF_SLOT;
        } else {
            int n = atoi(a);
            if (n < 1 || n > (int)APP_DRV_ADC_TOTAL_CH) {
                rt_kprintf("adc_trace: bad channel '%s'\n", a);
                return -1;
            }
            slots[c] = adc_rows[n - 1].adc_idx;
        }
    }

    rt_kprintf("frame");
    for (uint32_t c = 0; c < ncols; c++) {
        rt_kprintf(",%s%s", (slots[c] == ADC_STAT_VREF_SLOT) ? "vref" : "ch",
                   (slots[c] == ADC_STAT_VREF_SLOT) ? "" : argv[2 + c]);
    }
    rt_kprintf("\n");

    rt_sem_control(&adc_sem, RT_IPC_CMD_RESET, (void *)0);
    for (uint32_t f = 0; f < frames; f++) {
        char line[64];
        int len;
        uint16_t vals[ADC_STAT_SLOTS];

        if (app_drv_adc_wait(100) != RT_EOK) {
            rt_kprintf("adc_trace: timeout at frame %u\n", f);
            return -1;
        }
        adc_frame_read(vals);
        len = rt_snprintf(line, sizeof(line), "%u", f);
        for (uint32_t c = 0; c < ncols; c++) {
            len += rt_snprintf(line + len, sizeof(line) - (rt_size_t)len, ",%u", vals[slots[c]]);
        }
        rt_kprintf("%s\n", line);
    }
    rt_kprintf("adc_trace: done frames=%u\n", frames);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_adc_trace, adc_trace, CSV time series of selected ADC channels);
#endif
