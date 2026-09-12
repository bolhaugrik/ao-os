; Megszakitas- es kivetel-stubok mind a 256 vektorra.
; Minden stub egyseges keretet epit (struct regs a cpu/idt.h-ban), majd isr_dispatch(regs*).

bits 64
extern isr_dispatch

section .text

%macro STUB 1
isr_stub_%1:
%if (%1 == 8) || (%1 == 10) || (%1 == 11) || (%1 == 12) || (%1 == 13) || (%1 == 14) || (%1 == 17) || (%1 == 21) || (%1 == 29) || (%1 == 30)
    ; a CPU mar tett hibakodot a veremre
%else
    push qword 0
%endif
    push qword %1
    jmp isr_common
%endmacro

%assign i 0
%rep 256
    STUB i
%assign i i+1
%endrep

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp
    cld
    call isr_dispatch
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16             ; vektor + hibakod
    iretq

; GDT betoltese es szegmensek ujratoltese (rdi = gdtr cime, rsi = TSS szelektor)
global gdt_load
gdt_load:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax
    push 0x08
    lea rax, [rel .reload]
    push rax
    retfq
.reload:
    mov ax, si
    ltr ax
    ret

global idt_load
idt_load:
    lidt [rdi]
    ret

section .rodata
global isr_table
isr_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep
