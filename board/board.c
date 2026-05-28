/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2020-8-6       NU-LL        first version
 */

#include "board.h"

DMA_HandleTypeDef hdma_adc1;
DMA_HandleTypeDef hdma_adc3;

#ifdef RT_USING_MEMHEAP
#define AXI_SRAM_ADDR (0X24000000)
#define AXI_SRAM_SIZE (512*1024)
#define SRAM1_ADDR (0X30000000)
#define SRAM1_SIZE (128*1024)
#define SRAM2_ADDR (0X30020000)
#define SRAM2_SIZE (128*1024)
#define SRAM3_ADDR (0X30040000)
#define SRAM3_SIZE (32*1024)
#define SRAM4_ADDR (0X38000000)
#define SRAM4_SIZE (64*1024)
#define BACKUP_ADDR (0X38800000)
#define BACKUP_SIZE (4*1024)

static struct rt_memheap _heap_axi_sram;
static struct rt_memheap _heap_sram1;
static struct rt_memheap _heap_sram2;
static struct rt_memheap _heap_sram3;
static struct rt_memheap _heap_sram4;
static struct rt_memheap _heap_backup_sram;
#endif

static void PeriphCommonClock_Config(void)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInitStruct.PLL2.PLL2M = 2;
  PeriphClkInitStruct.PLL2.PLL2N = 12;
  PeriphClkInitStruct.PLL2.PLL2P = 2;
  PeriphClkInitStruct.PLL2.PLL2Q = 2;
  PeriphClkInitStruct.PLL2.PLL2R = 2;
  PeriphClkInitStruct.PLL2.PLL2RGE = RCC_PLL2VCIRANGE_3;
  PeriphClkInitStruct.PLL2.PLL2VCOSEL = RCC_PLL2VCOMEDIUM;
  PeriphClkInitStruct.PLL2.PLL2FRACN = 0;
  PeriphClkInitStruct.AdcClockSelection = RCC_ADCCLKSOURCE_PLL2;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 5;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 15;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }

  PeriphCommonClock_Config();
}

#ifdef RT_USING_MEMHEAP
static int init_sram(void)
{
    __HAL_RCC_D2SRAM1_CLK_ENABLE();
    __HAL_RCC_D2SRAM2_CLK_ENABLE();
    __HAL_RCC_D2SRAM3_CLK_ENABLE();
    rt_memheap_init(&_heap_axi_sram, "axi_sram", (void *)AXI_SRAM_ADDR, AXI_SRAM_SIZE);
    rt_memheap_init(&_heap_sram1, "sram1", (void *)SRAM1_ADDR, SRAM1_SIZE);
    rt_memheap_init(&_heap_sram2, "sram2", (void *)SRAM2_ADDR, SRAM2_SIZE);
    rt_memheap_init(&_heap_sram3, "sram3", (void *)SRAM3_ADDR, SRAM3_SIZE);
    rt_memheap_init(&_heap_sram4, "sram4", (void *)SRAM4_ADDR, SRAM4_SIZE);
    rt_memheap_init(&_heap_backup_sram, "bak_sram", (void *)BACKUP_ADDR, BACKUP_SIZE);

    return 0;
}
INIT_BOARD_EXPORT(init_sram);
#endif

/* MPU region 0 covers the QSPI XIP window so the CPU can fetch & cache code
 * from external W25Q64 efficiently. The bootloader already sets this up
 * before jumping, but configure it here too so a soft reset or
 * direct-to-app debug session still gets the right attributes. */
static void mpu_config_qspi_xip(void)
{
    MPU_Region_InitTypeDef region = {0};

    HAL_MPU_Disable();

    region.Enable           = MPU_REGION_ENABLE;
    region.Number           = MPU_REGION_NUMBER0;
    region.BaseAddress      = ROM_START;
    region.Size             = MPU_REGION_SIZE_8MB;
    region.AccessPermission = MPU_REGION_FULL_ACCESS;
    region.IsBufferable     = MPU_ACCESS_BUFFERABLE;
    region.IsCacheable      = MPU_ACCESS_CACHEABLE;
    region.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
    region.TypeExtField     = MPU_TEX_LEVEL1;
    region.SubRegionDisable = 0x00;
    region.DisableExec      = MPU_INSTRUCTION_ACCESS_ENABLE;
    HAL_MPU_ConfigRegion(&region);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

/* Override the rt_weak rt_hw_board_init in drv_common.c. Identical body
 * except for the MPU + VTOR fixup that has to run before HAL_Init() (which
 * enables SysTick and from then on needs the right vector table). */
void rt_hw_board_init(void)
{
    mpu_config_qspi_xip();

    /* Relocate the vector table to the start of the QSPI XIP image. */
    SCB->VTOR = ROM_START;
    __DSB();
    __ISB();

#ifdef BSP_SCB_ENABLE_I_CACHE
    SCB_EnableICache();
#endif

#ifdef BSP_SCB_ENABLE_D_CACHE
    SCB_EnableDCache();
#endif

    HAL_Init();
    SystemClock_Config();

#if defined(RT_USING_HEAP)
    rt_system_heap_init((void *)HEAP_BEGIN, (void *)HEAP_END);
#endif

#ifdef RT_USING_PIN
    rt_hw_pin_init();
#endif

#ifdef RT_USING_SERIAL
    extern int rt_hw_usart_init(void);
    rt_hw_usart_init();
#endif

#if defined(RT_USING_CONSOLE) && defined(RT_USING_DEVICE)
    rt_console_set_device(RT_CONSOLE_DEVICE_NAME);
#endif

#ifdef RT_USING_COMPONENTS_INIT
    rt_components_board_init();
#endif
}
