/*
 * Open Flashloader implementation for STM32H750 + W25Q64JV. Pin mapping
 * matches the bootloader; readback uses 1-4-4 memory-mapped QSPI, programming
 * uses 1-1-4 quad input page program.
 *
 * NOTE: J-Link executes this image from AXI-SRAM. No HAL, no libc, no
 * interrupts. Direct register pokes only.
 */
#include "FlashOS.h"

/* ---------- Register map ---------- */
#define RCC_BASE        0x58024400U
#define RCC_AHB3ENR     (*(volatile U32 *)(RCC_BASE + 0xD4))
#define RCC_AHB4ENR     (*(volatile U32 *)(RCC_BASE + 0xE0))

#define GPIOB_BASE      0x58020400U
#define GPIOD_BASE      0x58020C00U
#define GPIOE_BASE      0x58021000U
#define GPIO_MODER(b)   (*(volatile U32 *)((b) + 0x00))
#define GPIO_OSPEEDR(b) (*(volatile U32 *)((b) + 0x08))
#define GPIO_PUPDR(b)   (*(volatile U32 *)((b) + 0x0C))
#define GPIO_AFRL(b)    (*(volatile U32 *)((b) + 0x20))
#define GPIO_AFRH(b)    (*(volatile U32 *)((b) + 0x24))

#define QSPI_BASE       0x52005000U
#define QSPI_CR         (*(volatile U32 *)(QSPI_BASE + 0x00))
#define QSPI_DCR        (*(volatile U32 *)(QSPI_BASE + 0x04))
#define QSPI_SR         (*(volatile U32 *)(QSPI_BASE + 0x08))
#define QSPI_FCR        (*(volatile U32 *)(QSPI_BASE + 0x0C))
#define QSPI_DLR        (*(volatile U32 *)(QSPI_BASE + 0x10))
#define QSPI_CCR        (*(volatile U32 *)(QSPI_BASE + 0x14))
#define QSPI_AR         (*(volatile U32 *)(QSPI_BASE + 0x18))
#define QSPI_ABR        (*(volatile U32 *)(QSPI_BASE + 0x1C))
#define QSPI_DR         (*(volatile U32 *)(QSPI_BASE + 0x20))
#define QSPI_DR_BYTE    (*(volatile U8  *)(QSPI_BASE + 0x20))

#define SR_BUSY         (1U << 5)
#define SR_FTF          (1U << 2)
#define SR_TCF          (1U << 1)
#define SR_TEF          (1U << 0)
#define SR_FLEVEL_MASK  (0x1FU << 8)
#define SR_FLEVEL_SHIFT 8

/* CCR fields */
#define CCR_FMODE_WR    (0U << 26)
#define CCR_FMODE_RD    (1U << 26)
#define CCR_FMODE_MM    (3U << 26)
#define CCR_DMODE_1L    (1U << 24)
#define CCR_DMODE_4L    (3U << 24)
#define CCR_DCYC(n)     (((U32)(n) & 0x1F) << 18)
#define CCR_ABSIZE_8    (0U << 16)
#define CCR_ABMODE_4L   (3U << 14)
#define CCR_ADSIZE_24   (2U << 12)
#define CCR_ADMODE_1L   (1U << 10)
#define CCR_ADMODE_4L   (3U << 10)
#define CCR_IMODE_1L    (1U << 8)

/* W25Q opcodes */
#define CMD_WREN        0x06
#define CMD_RDSR1       0x05
#define CMD_RDSR2       0x35
#define CMD_WRSR2       0x31
#define CMD_JEDEC       0x9F
#define CMD_RST_EN      0x66
#define CMD_RST         0x99
#define CMD_QPP         0x32    /* 1-1-4 quad input page program, 256 B */
#define CMD_BE_64K      0xD8    /* block erase, 64 KB */
#define CMD_CE          0xC7    /* chip erase */
#define CMD_FAST_4IO    0xEB    /* 1-4-4 fast read quad I/O */

#define SR2_QE          (1U << 1)

/* ---------- Helpers ---------- */
static U32 qspi_mmap_active;

static void qspi_wait_busy(void)
{
    while (QSPI_SR & SR_BUSY) { }
}

static void qspi_clear_flags(void)
{
    QSPI_FCR = SR_TCF | SR_TEF;
}

static void qspi_abort(void)
{
    QSPI_CR |= (1U << 1);
    while (QSPI_CR & (1U << 1)) { }
    qspi_clear_flags();
    qspi_mmap_active = 0;
}

static void qspi_prepare_indirect(void)
{
    if (qspi_mmap_active) {
        qspi_abort();
    }
}

static U32 qspi_fifo_level(void)
{
    return (QSPI_SR & SR_FLEVEL_MASK) >> SR_FLEVEL_SHIFT;
}

static U32 load_le32(const U8 *p)
{
    return ((U32)p[0]) | ((U32)p[1] << 8) | ((U32)p[2] << 16) | ((U32)p[3] << 24);
}

/* Send a command with no address and no data. */
static int qspi_cmd(U8 opcode)
{
    qspi_prepare_indirect();
    qspi_wait_busy();
    qspi_clear_flags();
    QSPI_CCR = CCR_FMODE_WR | CCR_IMODE_1L | opcode;
    while (!(QSPI_SR & SR_TCF)) {
        if (QSPI_SR & SR_TEF) { qspi_clear_flags(); return -1; }
    }
    qspi_clear_flags();
    return 0;
}

/* Send opcode + address (24-bit) + optional data write. len <= 256. */
static int qspi_cmd_addr_write(U8 opcode, U32 addr, const U8 *data, U32 len, U32 data_mode)
{
    qspi_prepare_indirect();
    qspi_wait_busy();
    qspi_clear_flags();

    U32 ccr = CCR_FMODE_WR | CCR_IMODE_1L | CCR_ADMODE_1L | CCR_ADSIZE_24 | opcode;
    if (len) {
        ccr |= data_mode;
        QSPI_DLR = len - 1;
    }
    QSPI_CCR = ccr;
    QSPI_AR  = addr;

    U32 i = 0;
    while (i + 4 <= len) {
        while (qspi_fifo_level() > 28U) {
            if (QSPI_SR & SR_TEF) { qspi_clear_flags(); return -2; }
        }
        QSPI_DR = load_le32(data + i);
        i += 4;
    }
    while (i < len) {
        while (qspi_fifo_level() >= 32U) {
            if (QSPI_SR & SR_TEF) { qspi_clear_flags(); return -2; }
        }
        QSPI_DR_BYTE = data[i];
        i++;
    }
    while (!(QSPI_SR & SR_TCF)) {
        if (QSPI_SR & SR_TEF) { qspi_clear_flags(); return -3; }
    }
    qspi_clear_flags();
    return 0;
}

/* Read register-style: opcode + N data bytes. */
static int qspi_read_reg(U8 opcode, U8 *out, U32 len)
{
    qspi_prepare_indirect();
    qspi_wait_busy();
    qspi_clear_flags();
    QSPI_DLR = len - 1;
    QSPI_CCR = CCR_FMODE_RD | CCR_IMODE_1L | CCR_DMODE_1L | opcode;
    for (U32 i = 0; i < len; i++) {
        while (!(QSPI_SR & SR_FTF)) {
            if (QSPI_SR & SR_TEF) { qspi_clear_flags(); return -1; }
        }
        out[i] = QSPI_DR_BYTE;
    }
    while (!(QSPI_SR & SR_TCF)) {
        if (QSPI_SR & SR_TEF) { qspi_clear_flags(); return -2; }
    }
    qspi_clear_flags();
    return 0;
}

/* Spin until WIP=0 in SR1. */
static int wait_wip_clear(U32 spins)
{
    for (U32 i = 0; i < spins; i++) {
        U8 sr1 = 0;
        if (qspi_read_reg(CMD_RDSR1, &sr1, 1) != 0) return -1;
        if ((sr1 & 0x01) == 0) return 0;
    }
    return -2;
}

/* ---------- GPIO / QSPI bring-up ---------- */
static void gpio_set_af(U32 gpio_base, int pin, U8 af)
{
    int p = pin & 0x7;
    if (pin < 8) {
        U32 v = GPIO_AFRL(gpio_base);
        v &= ~(0xFU << (p * 4));
        v |=  ((U32)af << (p * 4));
        GPIO_AFRL(gpio_base) = v;
    } else {
        U32 v = GPIO_AFRH(gpio_base);
        v &= ~(0xFU << (p * 4));
        v |=  ((U32)af << (p * 4));
        GPIO_AFRH(gpio_base) = v;
    }

    U32 moder = GPIO_MODER(gpio_base);
    moder &= ~(0x3U << (pin * 2));
    moder |=  (0x2U << (pin * 2));     /* AF mode */
    GPIO_MODER(gpio_base) = moder;

    U32 ospeedr = GPIO_OSPEEDR(gpio_base);
    ospeedr |=  (0x3U << (pin * 2));   /* very high speed */
    GPIO_OSPEEDR(gpio_base) = ospeedr;
}

static void hardware_init(void)
{
    /* Enable GPIO clocks: B(1), D(3), E(4) */
    RCC_AHB4ENR |= (1U << 1) | (1U << 3) | (1U << 4);
    /* Enable QUADSPI clock */
    RCC_AHB3ENR |= (1U << 14);
    (void)RCC_AHB3ENR;

    gpio_set_af(GPIOB_BASE,  2,  9);    /* CLK   AF9  */
    gpio_set_af(GPIOB_BASE,  6, 10);    /* NCS   AF10 */
    gpio_set_af(GPIOD_BASE, 11,  9);    /* IO0   AF9  */
    gpio_set_af(GPIOD_BASE, 12,  9);    /* IO1   AF9  */
    gpio_set_af(GPIOE_BASE,  2,  9);    /* IO2   AF9  */
    gpio_set_af(GPIOD_BASE, 13,  9);    /* IO3   AF9  */

    /* If a previous run (bootloader) left QSPI memory-mapped, abort first. */
    qspi_mmap_active = 0;
    QSPI_CR  = 0;
    qspi_abort();

    /* PRESCALER=1. After J-Link's reset the H7 normally runs from 64 MHz HSI,
     * so QSPI is 32 MHz; if the clock tree is already up this is still within
     * the W25Q64JV's 133 MHz read/program command limit on this board. */
    QSPI_CR  = (1U << 24) | (1U << 4) | (1U << 3);
    /* FSIZE=22 (8 MB), CSHT=3 (4 cycles), CKMODE=0 */
    QSPI_DCR = (22U << 16) | (3U << 8);
    /* Enable */
    QSPI_CR |= 1U;
}

/* ---------- FlashOS interface ----------
 * J-Link discovers these by symbol name, so they MUST survive --gc-sections. */
__attribute__((used))
int Init(U32 adr, U32 clk, U32 fnc)
{
    (void)adr; (void)clk; (void)fnc;

    hardware_init();

    /* W25Q64 soft reset in case bootloader left it mid-something. */
    qspi_cmd(CMD_RST_EN);
    qspi_cmd(CMD_RST);
    /* Give the chip ~50us to recover -- spin a bit, no SysTick available. */
    for (volatile int i = 0; i < 50000; i++) { }

    /* Probe JEDEC; tolerate failure to keep loader robust on non-W25Q boards. */
    U8 id[3] = {0};
    if (qspi_read_reg(CMD_JEDEC, id, 3) != 0) {
        return 1;
    }
    if (id[0] != 0xEF || id[1] != 0x40) {
        return 2;
    }

    /* Ensure QE bit -- not strictly needed for 1-1-1 program, but keeps the
     * flash consistent with how the bootloader expects to read it (1-4-4). */
    U8 sr2 = 0;
    if (qspi_read_reg(CMD_RDSR2, &sr2, 1) != 0) return 3;
    if ((sr2 & SR2_QE) == 0) {
        if (qspi_cmd(CMD_WREN) != 0) return 4;

        /* WRSR2 takes no address, single data byte on 1 line. */
        U8 v = (U8)(sr2 | SR2_QE);
        qspi_wait_busy();
        qspi_clear_flags();
        QSPI_DLR = 0;
        QSPI_CCR = CCR_FMODE_WR | CCR_IMODE_1L | CCR_DMODE_1L | CMD_WRSR2;
        while (!(QSPI_SR & SR_FTF)) {
            if (QSPI_SR & SR_TEF) { qspi_clear_flags(); return 5; }
        }
        QSPI_DR_BYTE = v;
        while (!(QSPI_SR & SR_TCF)) {
            if (QSPI_SR & SR_TEF) { qspi_clear_flags(); return 6; }
        }
        qspi_clear_flags();
        if (wait_wip_clear(200000) != 0) return 7;
    }

    return 0;
}

__attribute__((used))
int UnInit(U32 fnc)
{
    (void)fnc;
    /* Disable QSPI peripheral so the next image (or bootloader run) sees a
     * clean state. */
    qspi_abort();
    QSPI_CR = 0;
    return 0;
}

__attribute__((used))
int EraseSector(U32 adr)
{
    U32 off = adr - 0x90000000U;

    if (qspi_cmd(CMD_WREN) != 0) return 1;

    /* 64 KB block erase. The flash device descriptor advertises 64 KB sectors
     * so J-Link aligns affected ranges before calling us. */
    qspi_wait_busy();
    qspi_clear_flags();
    QSPI_CCR = CCR_FMODE_WR | CCR_IMODE_1L | CCR_ADMODE_1L | CCR_ADSIZE_24 | CMD_BE_64K;
    QSPI_AR  = off;
    while (!(QSPI_SR & SR_TCF)) {
        if (QSPI_SR & SR_TEF) { qspi_clear_flags(); return 2; }
    }
    qspi_clear_flags();

    /* W25Q64 64 KB block erase: tBE1 max ~2s. */
    if (wait_wip_clear(100000000) != 0) return 3;
    return 0;
}

__attribute__((used))
int EraseChip(void)
{
    if (qspi_cmd(CMD_WREN) != 0) return 1;
    if (qspi_cmd(CMD_CE) != 0) return 2;
    /* tCE ~20s worst case. */
    if (wait_wip_clear(200000000) != 0) return 3;
    return 0;
}

__attribute__((used))
int ProgramPage(U32 adr, U32 sz, U8 *buf)
{
    U32 off = adr - 0x90000000U;
    if (sz == 0) return 0;

    while (sz) {
        U32 chunk = 256U - (off & 0xFFU);
        if (chunk > sz) chunk = sz;

        if (qspi_cmd(CMD_WREN) != 0) return 2;
        if (qspi_cmd_addr_write(CMD_QPP, off, buf, chunk, CCR_DMODE_4L) != 0) return 3;
        if (wait_wip_clear(2000000) != 0) return 4;

        off += chunk;
        buf += chunk;
        sz  -= chunk;
    }
    return 0;
}

static int qspi_enter_memory_mapped_read(void)
{
    if (qspi_mmap_active) {
        return 0;
    }

    qspi_wait_busy();
    qspi_clear_flags();

    /* Same read mode as the bootloader: 0xEB 1-4-4, 8-bit mode byte on 4
     * lines, then 4 additional dummy cycles. */
    QSPI_ABR = 0xF0U;
    QSPI_CCR = CCR_FMODE_MM | CCR_IMODE_1L | CCR_ADMODE_4L | CCR_ADSIZE_24 |
               CCR_ABMODE_4L | CCR_ABSIZE_8 | CCR_DCYC(4) |
               CCR_DMODE_4L | CMD_FAST_4IO;
    qspi_mmap_active = 1;
    return 0;
}

static void copy_from_mmap(U32 adr, U32 sz, U8 *buf)
{
    const volatile U8 *src8 = (const volatile U8 *)adr;

    while (sz && ((((U32)src8 | (U32)buf) & 3U) != 0U)) {
        *buf++ = *src8++;
        sz--;
    }

    const volatile U32 *src32 = (const volatile U32 *)src8;
    U32 *dst32 = (U32 *)buf;
    while (sz >= 16U) {
        dst32[0] = src32[0];
        dst32[1] = src32[1];
        dst32[2] = src32[2];
        dst32[3] = src32[3];
        src32 += 4;
        dst32 += 4;
        sz -= 16U;
    }
    while (sz >= 4U) {
        *dst32++ = *src32++;
        sz -= 4U;
    }

    src8 = (const volatile U8 *)src32;
    buf = (U8 *)dst32;
    while (sz) {
        *buf++ = *src8++;
        sz--;
    }
}

/* SEGGER Open Flashloader extension. J-Link calls this when it needs to read
 * the flash during compare/verify phases. Without it, J-Link falls back to a
 * direct AHB read of 0x90000000, which returns garbage because Init() leaves
 * QSPI in indirect mode (not memory-mapped). Returns # bytes read on success,
 * negative on error. */
__attribute__((used))
int SEGGER_OPEN_Read(U32 adr, U32 sz, U8 *buf)
{
    if (sz == 0) return 0;

    if (qspi_enter_memory_mapped_read() != 0) return -1;
    copy_from_mmap(adr, sz, buf);
    return (int)sz;
}

__attribute__((used))
int BlankCheck(U32 adr, U32 sz, U8 pat)
{
    /* Optional. Returning 1 forces J-Link to read & compare itself, which
     * works because the QSPI is in indirect-read mode after Init() and the
     * driver provides its own read path. Implementing here would just slow us
     * down without benefit. */
    (void)adr; (void)sz; (void)pat;
    return 1;
}
