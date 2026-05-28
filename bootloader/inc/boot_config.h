#ifndef BOOT_CONFIG_H
#define BOOT_CONFIG_H

#define BOOT_VERSION_STR        "0.1"

#define APP_BASE_ADDR           0x90000000U     /* QSPI memory-mapped */
#define APP_REGION_SIZE         (8U * 1024U * 1024U)  /* W25Q64: 8MB */

#define BOOT_UART_BAUDRATE      115200U
#define BOOT_UART_INSTANCE      USART1

#define W25Q64_EXPECTED_JEDEC   0x00EF4017U     /* W25Q64JV: 0xEF 0x40 0x17 */

/* If non-zero, bootloader will hang in error-loop instead of jumping when
 * JEDEC ID mismatch is detected. */
#define BOOT_REQUIRE_JEDEC_OK   1

#endif /* BOOT_CONFIG_H */
