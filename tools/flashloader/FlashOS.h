/*
 * Minimal CMSIS-Pack / SEGGER Open Flashloader interface (Keil FlashOS API).
 * J-Link loads the resulting ELF (.FLM) into SRAM, locates the FlashDevice
 * struct by symbol, and calls Init/EraseSector/ProgramPage/UnInit via direct
 * PC-set + run.
 */
#ifndef FLASH_OS_H
#define FLASH_OS_H

#include <stdint.h>

typedef uint8_t  U8;
typedef uint16_t U16;
typedef uint32_t U32;

#define VERS        1                 /* Interface version */
#define FLASH_DRV_VERS  (0x0100 + VERS)

/* Device type */
#define UNKNOWN     0
#define ONCHIP      1
#define EXT8BIT     2
#define EXT16BIT    3
#define EXT32BIT    4
#define EXTSPI      5

#define SECTOR_NUM  512

struct FlashSectors {
    U32 szSector;       /* sector size in bytes              */
    U32 AddrSector;     /* sector start offset within device */
};

#define SECTOR_END  { 0xFFFFFFFF, 0xFFFFFFFF }

struct FlashDevice {
    U16 Vers;
    U8  DevName[128];
    U16 DevType;
    U32 DevAdr;
    U32 szDev;
    U32 szPage;
    U32 Res;
    U8  valEmpty;
    U32 toProg;
    U32 toErase;
    struct FlashSectors sectors[SECTOR_NUM];
};

/* Public flash-algo entry points (called by J-Link). Marked "used" so that
 * --gc-sections does not strip them: nothing inside the loader references
 * them, only the debugger does, by symbol lookup. */
#define KEPT __attribute__((used))

int Init        (U32 adr, U32 clk, U32 fnc) KEPT;
int UnInit      (U32 fnc)                   KEPT;
int EraseSector (U32 adr)                   KEPT;
int ProgramPage (U32 adr, U32 sz, U8 *buf)  KEPT;
int EraseChip   (void)                      KEPT;
int BlankCheck  (U32 adr, U32 sz, U8 pat)   KEPT;
int SEGGER_OPEN_Read    (U32 adr, U32 sz, U8 *buf)                 KEPT;

#endif /* FLASH_OS_H */
