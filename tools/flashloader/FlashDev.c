#include "FlashOS.h"

/* W25Q64JV layout: uniform 4 KB sectors over 8 MB, 256-byte program pages. */
struct FlashDevice const FlashDevice __attribute__((used, section(".PrgData"))) = {
    FLASH_DRV_VERS,                 /* Vers */
    "STM32H750 W25Q64JV QSPI",      /* DevName (<= 128 chars) */
    EXTSPI,                         /* DevType */
    0x90000000U,                    /* DevAdr -- QSPI memory-mapped base */
    0x00800000U,                    /* szDev  -- 8 MB */
    0x00000100U,                    /* szPage -- 256 B page program */
    0,                              /* Res */
    0xFF,                           /* valEmpty -- erased byte */
    400,                            /* toProg  ms */
    4000,                           /* toErase ms (sector) */
    {
        { 0x00001000U, 0x00000000U },   /* 4 KB sectors starting at offset 0 */
        SECTOR_END,
    }
};
