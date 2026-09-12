#pragma once
#include "types.h"

/* A stub altal felepitett keret (isr.asm), alacsony cimtol felfele. */
struct regs {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector, err;
    u64 rip, cs, rflags, rsp, ss;
};

typedef void (*irq_handler_fn)(struct regs *r);

#define IRQ_BASE 32

void idt_init(void);
void irq_register(u8 irq, irq_handler_fn fn);
