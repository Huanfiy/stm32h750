/*
 * Open Flashloader implementation for STM32H750 + W25Q64JV (1-line SPI
 * commands via QUADSPI indirect mode). Pin mapping matches the bootloader.
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
#define QSPI_DR         (*(volatile U32 *)(QSPI_BASE + 0x20))
#define QSPI_DR_BYTE    (*(volatile U8  *)(QSPI_BASE + 0x20))

#define SR_BUSY         (1U << 5)
#define SR_FTF          (1U << 2)
#define SR_TCF          (1U << 1)
#define SR_TEF          (1U << 0)

/* CCR fields */
#define CCR_FMODE_WR    (0U << 26)
#define CCR_FMODE_RD    (1U << 26)
#define CCR_DMODE_1L    (1U << 24)
#define CCR_DCYC(n)     (((U32)(n) & 0x1F) << 18)
#define CCR_ADSIZE_24   (2U << 12)
#define CCR_ADMODE_1L   (1U << 10)
#define CCR_IMODE_1L    (1U << 8)

/* W25Q opcodes */
#define CMD_WREN        0x06
#define CMD_RDSR1       0x05
#define CMD_RDSR2       0x35
#define CMD_WRSR2       0x31
#define CMD_JEDEC       0x9F
#define CMD_RST_EN      0x66
#define CMD_RST         0x99
#define CMD_PP          0x02    /* 1-1-1 page program, 256 B */
#define CMD_SE_4K       0x20    /* sector erase, 4 KB */
#define CMD_CE          0xC7    /* chip erase */

#define SR2_QE          (1U << 1)

/* ---------- Helpers ---------- */
static void qspi_wait_busy(void)
{
    while (QSPI_SR & SR_BUSY) { }
}

static void qspi_clear_flags(void)
{
    QSPI_FCR = SR_TCF | SR_TEF;
}

/* Send a command with no address and no data. */
static int qspi_cmd(U8 opcode)
{
    qspi_wait_busy();
    qspi_clear_flags();
    QSPI_CCR = CCR_FMODE_WR | CCR_IMODE_1L | opcode;
    while (!(QSPI_SR & SR_TCF)) {
        if (QSPI_SR & SR_TEF) { qspi_clear_flags(); return -1; }
    }
    qspi_clear_flags();
    return 0;
}

/* Send opcode + address (24-bit) + optional 1-line data write. len <= 256. */
static int qspi_cmd_addr_write(U8 opcode, U32 addr, const U8 *data, U32 len)
{
    qspi_wait_busy();
    qspi_clear_flags();

    U32 ccr = CCR_FMODE_WR | CCR_IMODE_1L | CCR_ADMODE_1L | CCR_ADSIZE_24 | opcode;
    if (len) {
        ccr |= CCR_DMODE_1L;
        QSPI_DLR = len - 1;
    }
    QSPI_CCR = ccr;
    QSPI_AR  = addr;
    for (U32 i = 0; i < len; i++) {
        while (!(QSPI_SR & SR_FTF)) {
            if (QSPI_SR & SR_TEF) { qspi_clear_flags(); return -2; }
        }
        QSPI_DR_BYTE = data[i];
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
    QSPI_CR  = 0;
    QSPI_CR |= (1U << 1);   /* ABORT */
    while (QSPI_CR & (1U << 1)) { }
    qspi_clear_flags();

    /* PRESCALER=3 (D1HCLK/4 ~= 60 MHz assuming bootloader left 240 MHz; if the
     * core is at default HSI 64 MHz this becomes 16 MHz which is still fine).
     * SSHIFT=1, TCEN=1. */
    QSPI_CR  = (3U << 24) | (1U << 4) | (1U << 3);
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
    QSPI_CR = 0;
    return 0;
}

__attribute__((used))
int EraseSector(U32 adr)
{
    U32 off = adr - 0x90000000U;

    if (qspi_cmd(CMD_WREN) != 0) return 1;

    /* Sector erase 4 KB at offset `off`. */
    qspi_wait_busy();
    qspi_clear_flags();
    QSPI_CCR = CCR_FMODE_WR | CCR_IMODE_1L | CCR_ADMODE_1L | CCR_ADSIZE_24 | CMD_SE_4K;
    QSPI_AR  = off;
    while (!(QSPI_SR & SR_TCF)) {
        if (QSPI_SR & SR_TEF) { qspi_clear_flags(); return 2; }
    }
    qspi_clear_flags();

    /* W25Q64 sector erase: tSE max ~400ms. */
    if (wait_wip_clear(20000000) != 0) return 3;
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
    if (sz > 256) return 1;

    if (qspi_cmd(CMD_WREN) != 0) return 2;
    if (qspi_cmd_addr_write(CMD_PP, off, buf, sz) != 0) return 3;
    if (wait_wip_clear(2000000) != 0) return 4;
    return 0;
}

/* SEGGER Open Flashloader extension. J-Link calls this when it needs to read
 * the flash during compare/verify phases. Without it, J-Link falls back to a
 * direct AHB read of 0x90000000, which returns garbage because Init() leaves
 * QSPI in indirect mode (not memory-mapped). Returns # bytes read on success,
 * negative on error. */
__attribute__((used))
int SEGGER_OPEN_Read(U32 adr, U32 sz, U8 *buf)
{
    U32 off = adr - 0x90000000U;
    if (sz == 0) return 0;

    qspi_wait_busy();
    qspi_clear_flags();
    QSPI_DLR = sz - 1;
    /* Fast Read (0x0B): 1-1-1, 8 dummy cycles, 24-bit address. */
    QSPI_CCR = CCR_FMODE_RD | CCR_IMODE_1L | CCR_ADMODE_1L | CCR_ADSIZE_24 |
               CCR_DCYC(8) | CCR_DMODE_1L | 0x0B;
    QSPI_AR  = off;

    for (U32 i = 0; i < sz; i++) {
        while (!(QSPI_SR & SR_FTF)) {
            if (QSPI_SR & SR_TEF) { qspi_clear_flags(); return -1; }
            if ((QSPI_SR & SR_TCF) && !(QSPI_SR & SR_FTF) && i + 1 >= sz) break;
        }
        buf[i] = QSPI_DR_BYTE;
    }
    while (!(QSPI_SR & SR_TCF)) {
        if (QSPI_SR & SR_TEF) { qspi_clear_flags(); return -2; }
    }
    qspi_clear_flags();
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
