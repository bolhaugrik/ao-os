; Kontextusvaltas, task-inditas, syscall-belepes.
bits 64

extern isr_return
extern syscall_dispatch
extern current_kstack_top

section .text

; void switch_to(u64 *old_rsp, u64 new_rsp)
global switch_to
switch_to:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; uj task elso futasa: a kernel-vermen egy kesz megszakitas-keret van
global task_trampoline
task_trampoline:
    jmp isr_return

; syscall belepes: rcx = user rip, r11 = user rflags, IF=0 (SFMASK).
; Ugyanolyan struct regs keretet epitunk, mint a megszakitasok, igy a
; kernel egysegesen latja a user-allapotot.
global syscall_entry
syscall_entry:
    mov [rel saved_user_rsp], rsp
    mov rsp, [rel current_kstack_top]
    push qword 0x1B                 ; ss  (SEL_UDATA|3)
    push qword [rel saved_user_rsp] ; rsp
    push r11                        ; rflags
    push qword 0x23                 ; cs  (SEL_UCODE|3)
    push rcx                        ; rip
    push qword 0                    ; err
    push qword 0x80                 ; "vektor": syscall
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
    sti
    call syscall_dispatch
    cli
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
    add rsp, 16                     ; vektor + err
    pop rcx                         ; rip
    add rsp, 8                      ; cs
    pop r11                         ; rflags
    pop rsp                         ; user verem (ss-t nem kell)
    o64 sysret

section .bss
saved_user_rsp: resq 1
