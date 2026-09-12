/* IDT + kozponti megszakitas-elosztas. Kivetel -> panic dump, IRQ -> regisztralt kezelo. */
#include "idt.h"
#include "gdt.h"
#include "pic.h"
#include "panic.h"
#include "../lib/string.h"

struct idt_entry {
    u16 off_lo;
    u16 sel;
    u8  ist;
    u8  type;
    u16 off_mid;
    u32 off_hi;
    u32 res;
} PACKED;

struct idtr {
    u16 limit;
    u64 base;
} PACKED;

static struct idt_entry idt[256] ALIGNED(16);
static irq_handler_fn irq_handlers[16];

extern const u64 isr_table[256];
extern void idt_load(const struct idtr *);

static void set_gate(u8 v, u64 handler, u8 ist)
{
    idt[v].off_lo = handler & 0xFFFF;
    idt[v].sel = SEL_KCODE;
    idt[v].ist = ist;
    idt[v].type = 0x8E;         /* jelen, DPL0, 64 bites interrupt gate */
    idt[v].off_mid = (handler >> 16) & 0xFFFF;
    idt[v].off_hi = handler >> 32;
    idt[v].res = 0;
}

void idt_init(void)
{
    memset(idt, 0, sizeof idt);
    for (int v = 0; v < 256; v++)
        set_gate((u8)v, isr_table[v], v == 8 ? 1 : 0);
    struct idtr r = { sizeof idt - 1, (u64)(uptr)idt };
    idt_load(&r);
}

void irq_register(u8 irq, irq_handler_fn fn)
{
    irq_handlers[irq & 15] = fn;
}

static const char *exc_name[32] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range",
    "invalid opcode", "device not available", "double fault", "coprocessor overrun",
    "invalid TSS", "segment not present", "stack fault", "general protection",
    "page fault", "reserved", "x87 FP", "alignment check", "machine check",
    "SIMD FP", "virtualization", "control protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved", "hypervisor", "VMM comm",
    "security", "reserved",
};

void isr_dispatch(struct regs *r)
{
    if (r->vector < 32) {
        panic_exception(exc_name[r->vector], r);
    }
    if (r->vector >= IRQ_BASE && r->vector < IRQ_BASE + 16) {
        u8 irq = (u8)(r->vector - IRQ_BASE);
        if (irq == 7 && !pic_irq_in_service(7)) {   /* hamis IRQ7 */
            return;
        }
        if (irq == 15 && !pic_irq_in_service(15)) { /* hamis IRQ15 */
            pic_eoi(2);
            return;
        }
        if (irq_handlers[irq])
            irq_handlers[irq](r);
        pic_eoi(irq);
        return;
    }
    /* egyeb vektor: figyelmen kivul */
}
