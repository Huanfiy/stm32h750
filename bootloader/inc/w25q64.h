#ifndef W25Q64_H
#define W25Q64_H

#include <stdint.h>

/* SPI command opcodes (W25Q64JV datasheet, Table 8.1.2) */
#define W25Q_CMD_WRITE_ENABLE       0x06
#define W25Q_CMD_READ_SR1           0x05
#define W25Q_CMD_READ_SR2           0x35
#define W25Q_CMD_WRITE_SR2          0x31
#define W25Q_CMD_JEDEC_ID           0x9F
#define W25Q_CMD_ENABLE_RESET       0x66
#define W25Q_CMD_RESET              0x99
#define W25Q_CMD_FAST_READ_QUAD_IO  0xEB

/* Status register bits */
#define W25Q_SR1_BUSY               (1U << 0)
#define W25Q_SR2_QE                 (1U << 1)

/* Returned by w25q64_read_jedec_id(); high byte zero, low 24 bits hold ID. */
uint32_t w25q64_read_jedec_id(void);

/* Soft reset (0x66 followed by 0x99). Used to clear residual QPI state on
 * warm boot. */
int w25q64_reset(void);

/* Make sure the QE bit (Status Register 2 bit 1) is set. */
int w25q64_enable_qe(void);

/* Configure QUADSPI into memory-mapped 1-4-4 mode using Fast Read Quad I/O
 * (0xEB). After this returns successfully, the CPU can read from
 * 0x90000000..0x90800000 as if it were on-chip flash. */
int w25q64_enter_memory_mapped(void);

#endif /* W25Q64_H */
