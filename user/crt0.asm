; AOX fejlec + belepes. A linker-script (user/aox.ld) ezt teszi a kep elejere.
bits 64

extern main
extern __load_end
extern __bss_size

section .aox_header
    dd 0x31584F41           ; 'AOX1'
    dd 1                    ; version
    dd _start               ; entry (a kep 0-tol linkelt, ezert ez az eltolas)
    dd __load_end           ; load_size
    dd __bss_size           ; bss_size
    dd 16384                ; stack_size (Phase 2)
    dd 0                    ; flags
    dd 0

section .text
global _start
_start:
    ; rdi = api, esi = argc, rdx = argv (kernel modban hivva, normal fuggvenykent)
    jmp main
