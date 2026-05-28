/*
 * Linker script for STM32H750 bootloader (internal flash @ 0x08000000, 128KB).
 * Vector table at start of .text; .data/.bss/.heap/.stack in DTCM @ 0x20000000.
 */

MEMORY
{
    ROM (rx) : ORIGIN = 0x08000000, LENGTH = 128k
    RAM (rw) : ORIGIN = 0x20000000, LENGTH = 128k
}

ENTRY(Reset_Handler)

_system_stack_size = 0x800;     /* 2 KB main stack for bootloader */

SECTIONS
{
    .text :
    {
        . = ALIGN(4);
        _stext = .;
        KEEP(*(.isr_vector))
        . = ALIGN(4);
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata*)
        *(.glue_7)
        *(.glue_7t)
        *(.gnu.linkonce.t*)
        . = ALIGN(4);
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);

        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
        . = ALIGN(4);

        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
        . = ALIGN(4);

        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
        . = ALIGN(4);
        _etext = .;
    } > ROM = 0

    __exidx_start = .;
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        _sidata = .;
    } > ROM
    __exidx_end = .;

    .data : AT (_sidata)
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data.*)
        *(.gnu.linkonce.d*)
        . = ALIGN(4);
        _edata = .;
    } > RAM

    .stack :
    {
        . = ALIGN(8);
        _sstack = .;
        . = . + _system_stack_size;
        . = ALIGN(8);
        _estack = .;
    } > RAM

    __bss_start = .;
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM
    __bss_end = .;

    _end = .;

    /DISCARD/ :
    {
        *(.ARM.attributes)
        *(.comment)
    }
}
