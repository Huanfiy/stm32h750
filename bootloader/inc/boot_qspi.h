#ifndef BOOT_QSPI_H
#define BOOT_QSPI_H

#include "stm32h7xx_hal.h"

/* Shared QSPI handle used by both the bootloader and HAL_QSPI_MspInit. */
extern QSPI_HandleTypeDef hqspi;

/* Initialize QUADSPI peripheral for W25Q64 (8MB).
 * Sets FlashSize = 22 (=> 2^23 = 8MB), clock prescaler 1 (HCLK/2),
 * sample shifting half cycle, CS high time 4 cycles. */
int boot_qspi_init(void);

#endif /* BOOT_QSPI_H */
