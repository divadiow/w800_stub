OUTPUT_ARCH(csky)
ENTRY(Reset_Handler)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x20004000, LENGTH = 0x00044000
}

SECTIONS
{
    . = ORIGIN(RAM);
    .vectors : ALIGN(1024)
    {
        KEEP(*(.vectors))
    } > RAM

    .text : ALIGN(4)
    {
        *(.text.startup*)
        *(.text*)
        *(.rodata*)
    } > RAM

    .data : ALIGN(4)
    {
        *(.data*)
    } > RAM

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    _end = .;

    ASSERT(_end <= ORIGIN(RAM) + LENGTH(RAM) - 0x2000,
           "stub leaves less than 8 KiB for the stack")
}
