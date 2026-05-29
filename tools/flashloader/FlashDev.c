#include "FlashOS.h"

/* W25Q64JV layout optimized for full-image app downloads: J-Link sends larger
 * program chunks and erases 64 KB blocks; ProgramPage still splits writes on
 * the chip's physical 256-byte page boundaries. */
struct FlashDevice const FlashDevice __attribute__((used, section(".PrgData"))) = {
    FLASH_DRV_VERS,                 /* Vers */
    "STM32H750 W25Q64JV QSPI",      /* DevName (<= 128 chars) */
    EXTSPI,                         /* DevType */
    0x90000000U,                    /* DevAdr -- QSPI memory-mapped base */
    0x00800000U,                    /* szDev  -- 8 MB */
    0x00001000U,                    /* szPage -- J-Link chunk size */
    0,                              /* Res */
    0xFF,                           /* valEmpty -- erased byte */
    400,                            /* toProg  ms */
    4000,                           /* toErase ms (sector) */
    {
        { 0x00010000U, 0x00000000U },   /* 64 KB erase blocks */
        SECTOR_END,
    }
};
