#ifndef BOOT_UART_H
#define BOOT_UART_H

#include <stdint.h>

void boot_uart_init(void);
void boot_uart_write(const char *buf, uint32_t len);
void boot_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* BOOT_UART_H */
