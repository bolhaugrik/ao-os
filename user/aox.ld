/* AOX lapos kep a rogzitett USER_LOAD (0x400000) cimre linkelve. A kod RIP-relativ,
 * de az adatokban levo abszolut mutatok (pl. a fordito switch-tablai, mutato-tombok)
 * csak igy lesznek helyesek: relokacio-feldolgozas nincs a betoltoben. */
ENTRY(_start)
__aox_base = 0x400000;
SECTIONS
{
    . = __aox_base;
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
