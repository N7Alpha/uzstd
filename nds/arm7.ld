ENTRY(_start)

MEMORY
{
    ram : ORIGIN = 0x03800000, LENGTH = 64K
}

SECTIONS
{
    . = ORIGIN(ram);

    .text : ALIGN(4)
    {
        KEEP(*(.init))
        *(.text*)
        *(.rodata*)
    } > ram

    .data : ALIGN(4)
    {
        *(.data*)
    } > ram

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > ram
}
