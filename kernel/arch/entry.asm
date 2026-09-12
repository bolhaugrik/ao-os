; AO-OS kernel belepesi pont.
; A stage2 fizikai 0x100000-ra ugrik ide (identity map), RDI = bootinfo fizikai cime.
; Ez a szakasz a .text.entry-be kerul, a linker-script az image elejere teszi.

bits 64

extern kmain
extern __bss_start
extern __bss_end

DIRECT_MAP_BASE equ 0xFFFF800000000000

section .text.entry
global _start
_start:
    ; atugras a higher-half virtualis cimre
    mov rax, .high
    jmp rax
.high:
    lea rsp, [rel kstack_top]
    xor rbp, rbp

    ; bss nullazasa. A verem is a bss-ben van, ezert az RDI-t regiszterben
    ; orizzuk, nem a veremben (a stosq felulirna a mentett erteket).
    mov rbx, rdi
    lea rdi, [rel __bss_start]
    lea rcx, [rel __bss_end]
    sub rcx, rdi
    shr rcx, 3
    xor eax, eax
    rep stosq
    mov rdi, rbx

    ; bootinfo fizikai -> direkt map virtualis
    mov rax, DIRECT_MAP_BASE
    add rdi, rax

    call kmain
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
kstack:
    resb 16384
kstack_top:
