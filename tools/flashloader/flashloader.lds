/*
 * J-Link Open Flashloader link script. The image is loaded by J-Link into
 * AXI-SRAM (0x24000000) and executed in place. No interrupts, no startup, no
 * relocations -- J-Link sets PC=Init/EraseSector/... and SP itself.
 */

ENTRY(Init)

/* Symbols J-Link discovers by name only. EXTERN forces ld to treat them as
 * roots so --gc-sections does not drop them. */
EXTERN(Init UnInit EraseSector EraseChip ProgramPage BlankCheck SEGGER_OPEN_Read FlashDevice)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x24000000, LENGTH = 32K
}

SECTIONS
{
    PrgCode 0x24000000 :
    {
        KEEP(*(.PrgCode*))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > RAM

    PrgData :
    {
        *(.data*)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
    } > RAM

    DevDscr :
    {
        KEEP(*(.PrgData))   /* FlashDevice struct */
        . = ALIGN(4);
    } > RAM

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.attributes)
        *(.comment)
        *(.note*)
    }
}
