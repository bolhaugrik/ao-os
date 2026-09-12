/* AOX lapos kep: 0-tol linkelve, RIP-relativ kod, a betoltesi cim tetszoleges. */
ENTRY(_start)
SECTIONS
{
    . = 0;
    .aox_header : { KEEP(*(.aox_header)) }
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    . = ALIGN(16);
    __load_end = .;
    .bss : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(16);
    __bss_size = . - __load_end;
    /DISCARD/ : { *(.eh_frame .eh_frame_hdr .note.* .comment) }
}
