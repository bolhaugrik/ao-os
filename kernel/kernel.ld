/* AO-OS kernel linker-script: higher-half, -2 GiB, fizikai 1 MiB-re toltve. */
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80100000;
KERNEL_LMA = 0x100000;

SECTIONS
{
    . = KERNEL_VMA;
    __kernel_start = .;

    .text : AT(ADDR(.text) - KERNEL_VMA + KERNEL_LMA)
    {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }

    . = ALIGN(4096);
    .rodata : AT(ADDR(.rodata) - KERNEL_VMA + KERNEL_LMA)
    {
        *(.rodata .rodata.*)
    }

    . = ALIGN(4096);
    .data : AT(ADDR(.data) - KERNEL_VMA + KERNEL_LMA)
    {
        *(.data .data.*)
    }

    . = ALIGN(4096);
    .bss : AT(ADDR(.bss) - KERNEL_VMA + KERNEL_LMA)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    __kernel_end = .;

    /DISCARD/ :
    {
        *(.eh_frame .eh_frame_hdr .note.* .comment .debug_gdb_scripts)
    }
}
