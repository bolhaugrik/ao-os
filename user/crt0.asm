; AOX fejlec + belepes. A linker-script (user/aox.ld) ezt teszi a kep elejere.
; Ring 3-ban indul: rdi = argc, rsi = argv (a kernel a veremre masolta).
bits 64

extern main
extern ao_exit
extern __load_end
extern __bss_size

AOX_BASE equ 0x400000       ; = __aox_base az aox.ld-ben es USER_LOAD a kernelben

section .aox_header
    dd 0x31584F41           ; 'AOX1'
    dd 1                    ; version
    dd _start - AOX_BASE    ; entry (eltolas a kep elejetol)
    dd __load_end - AOX_BASE ; load_size
    dd __bss_size           ; bss_size
    dd 65536                ; stack_size
    dd 0                    ; flags
    dd 0

section .text
global _start
_start:
    xor rbp, rbp
    and rsp, -16            ; ABI: a call elott 16-ra igazitott verem (SSE-s kod is jo)
    call main
    mov edi, eax
    call ao_exit
.hang:
    jmp .hang
