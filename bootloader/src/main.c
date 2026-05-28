/* STM32H750VB bootloader: bring up QSPI W25Q64 (1-4-4 memory mapped) then
 * branch to the user application at 0x90000000. */

#include "stm32h7xx_hal.h"
#include "boot_config.h"
#include "boot_uart.h"
#include "boot_qspi.h"
#include "w25q64.h"

extern void Error_Handler(void);

static void SystemClock_Config(void);
static void MPU_Config(void);
static void fatal(const char *why);
static void jump_to_app(uint32_t addr);

int main(void)
{
    MPU_Config();
    HAL_Init();
    SystemClock_Config();

    boot_uart_init();
    boot_log("\r\n[BOOT] STM32H750 v%s -- %s %s\r\n",
             BOOT_VERSION_STR, __DATE__, __TIME__);

    if (boot_qspi_init() != 0)
    {
        fatal("qspi peripheral init failed");
    }
    boot_log("[BOOT] QSPI peripheral ready (FlashSize=22, 8MB)\r\n");

    /* Warm-boot guard: previous run may have left the part in QPI mode. We
     * cannot drive a QPI 0xFF here from 1-line state, so accept that warm
     * cases stuck in QPI need a power-cycle. Issue the 1-line soft reset
     * regardless to cover the common case. */
    (void)w25q64_reset();

    uint32_t jedec = w25q64_read_jedec_id();
    boot_log("[BOOT] JEDEC ID = 0x%06lX\r\n", (unsigned long)jedec);

    if (jedec != W25Q64_EXPECTED_JEDEC)
    {
#if BOOT_REQUIRE_JEDEC_OK
        fatal("JEDEC ID mismatch");
#else
        boot_log("[BOOT] WARN: JEDEC mismatch, proceeding anyway\r\n");
#endif
    }

    if (w25q64_enable_qe() != 0)
    {
        fatal("failed to set QE bit");
    }
    boot_log("[BOOT] QE bit ensured\r\n");

    if (w25q64_enter_memory_mapped() != 0)
    {
        fatal("memory-mapped switch failed");
    }
    boot_log("[BOOT] memory-mapped @ 0x%08lX ok\r\n",
             (unsigned long)APP_BASE_ADDR);

    uint32_t app_sp = *(volatile uint32_t *)APP_BASE_ADDR;
    uint32_t app_pc = *(volatile uint32_t *)(APP_BASE_ADDR + 4);
    boot_log("[BOOT] sniff: SP=0x%08lX PC=0x%08lX\r\n",
             (unsigned long)app_sp, (unsigned long)app_pc);

    /* Sanity: SP should point into RAM (0x20000000..0x20020000 DTCM) and PC
     * should fall inside the QSPI region with thumb bit set. */
    if ((app_sp & 0xFF000000U) != 0x20000000U ||
        (app_pc & 0xFF000000U) != 0x90000000U ||
        (app_pc & 1U) == 0)
    {
        boot_log("[BOOT] WARN: app header looks invalid; not jumping\r\n");
        while (1)
        {
            HAL_Delay(500);
            boot_log("[BOOT] idle\r\n");
        }
    }

    boot_log("[BOOT] jump 0x%08lX\r\n", (unsigned long)APP_BASE_ADDR);
    /* Drain UART before tearing down peripherals. */
    HAL_Delay(10);

    jump_to_app(APP_BASE_ADDR);

    /* Should never reach here. */
    while (1) { }
}

static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);
    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) { }

    osc.OscillatorType   = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState         = RCC_HSE_ON;
    osc.PLL.PLLState     = RCC_PLL_ON;
    osc.PLL.PLLSource    = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM         = 5;
    osc.PLL.PLLN         = 192;
    osc.PLL.PLLP         = 2;
    osc.PLL.PLLQ         = 15;
    osc.PLL.PLLR         = 2;
    osc.PLL.PLLRGE       = RCC_PLL1VCIRANGE_2;
    osc.PLL.PLLVCOSEL    = RCC_PLL1VCOWIDE;
    osc.PLL.PLLFRACN     = 0;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
    {
        Error_Handler();
    }

    clk.ClockType        = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                         | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2
                         | RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
    clk.SYSCLKSource     = RCC_SYSCLKSOURCE_PLLCLK;
    clk.SYSCLKDivider    = RCC_SYSCLK_DIV1;
    clk.AHBCLKDivider    = RCC_HCLK_DIV2;
    clk.APB3CLKDivider   = RCC_APB3_DIV2;
    clk.APB1CLKDivider   = RCC_APB1_DIV2;
    clk.APB2CLKDivider   = RCC_APB2_DIV2;
    clk.APB4CLKDivider   = RCC_APB4_DIV2;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK)
    {
        Error_Handler();
    }
}

/* Mark the QSPI memory-mapped window as Normal/Cacheable so the CPU can XIP
 * efficiently. Without this, the default MPU layout makes 0x90000000 a
 * Device-shareable region, which is unaligned-access-fault prone and slow. */
static void MPU_Config(void)
{
    MPU_Region_InitTypeDef region = {0};

    HAL_MPU_Disable();

    region.Enable           = MPU_REGION_ENABLE;
    region.Number           = MPU_REGION_NUMBER0;
    region.BaseAddress      = APP_BASE_ADDR;
    region.Size             = MPU_REGION_SIZE_8MB;
    region.AccessPermission = MPU_REGION_FULL_ACCESS;
    region.IsBufferable     = MPU_ACCESS_BUFFERABLE;
    region.IsCacheable      = MPU_ACCESS_CACHEABLE;
    region.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
    region.TypeExtField     = MPU_TEX_LEVEL1;       /* Normal, WB+WA combined with C/B */
    region.SubRegionDisable = 0x00;
    region.DisableExec      = MPU_INSTRUCTION_ACCESS_ENABLE;
    HAL_MPU_ConfigRegion(&region);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

static void fatal(const char *why)
{
    boot_log("[BOOT] FATAL: %s\r\n", why);
    while (1)
    {
        HAL_Delay(500);
        boot_log("[BOOT] halted\r\n");
    }
}

typedef void (*app_entry_t)(void);

static void jump_to_app(uint32_t addr)
{
    uint32_t sp = *(volatile uint32_t *)addr;
    uint32_t pc = *(volatile uint32_t *)(addr + 4);

    __disable_irq();

    /* Stop SysTick, mask & clear pending NVIC lines so the app starts with a
     * clean slate. */
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;
    for (int i = 0; i < 8; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    /* Hand off the vector table to the application's. */
    SCB->VTOR = addr;
    __DSB();
    __ISB();

    __set_MSP(sp);
    ((app_entry_t)pc)();
}

/* HAL needs a 1ms tick. Use SysTick directly via HAL's default handler
 * weak wiring. The startup file already provides SysTick_Handler -> while(1);
 * override it so HAL_IncTick gets called. */
void SysTick_Handler(void)
{
    HAL_IncTick();
}
