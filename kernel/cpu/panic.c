/* Panic: regiszter-dump a konzolra (ha van) es a soros portra, majd megallas.
 * Valodi HW-n egy fenykep a kepernyorol eleg a hibakereseshez. */
#include "panic.h"
#include "idt.h"
#include "../arch/io.h"
#include "../drv/console.h"
#include "../drv/serial.h"
#include "../fs/disk.h"
#include "../lib/fmt.h"

static void out(char c, void *ctx)
{
    (void)ctx;
    if (console_ready())
        console_putc(c);
    else
        serial_putc(c);
}

static void pprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vformat(out, NULL, fmt, ap);
    va_end(ap);
}

static u64 read_cr2(void) { u64 v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static u64 read_cr3(void) { u64 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }

NORETURN void panic(const char *fmt, ...)
{
    cli();
    if (console_ready())
        console_set_color(CON_RED, CON_BLACK);
    pprintf("\n*** PANIC: ");
    va_list ap;
    va_start(ap, fmt);
    vformat(out, NULL, fmt, ap);
    va_end(ap);
    pprintf("\n");
    if (console_ready())
        console_flush();
    halt_forever();
}

/* a dump rovid valtozata a panic-tarolo szektorba (512 B) */
static void store_dump(const char *name, struct regs *r)
{
    char buf[500];
    snformat(buf, sizeof buf,
             "KIVETEL %lu %s err=%lx rip=%lx rsp=%lx cr2=%lx rax=%lx rbx=%lx rcx=%lx rdx=%lx rsi=%lx rdi=%lx rbp=%lx",
             r->vector, name, r->err, r->rip, r->rsp, read_cr2(), r->rax, r->rbx, r->rcx, r->rdx, r->rsi, r->rdi, r->rbp);
    panic_store_write(buf);
}

NORETURN void panic_exception(const char *name, struct regs *r)
{
    cli();
    if (console_ready())
        console_set_color(CON_RED, CON_BLACK);
    pprintf("\n*** KIVETEL %lu: %s  err=0x%lx\n", r->vector, name, r->err);
    pprintf("rip=%016lx cs=%04lx rflags=%08lx rsp=%016lx ss=%04lx\n", r->rip, r->cs, r->rflags, r->rsp, r->ss);
    pprintf("rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx\n", r->rax, r->rbx, r->rcx, r->rdx);
    pprintf("rsi=%016lx rdi=%016lx rbp=%016lx r8 =%016lx\n", r->rsi, r->rdi, r->rbp, r->r8);
    pprintf("r9 =%016lx r10=%016lx r11=%016lx r12=%016lx\n", r->r9, r->r10, r->r11, r->r12);
    pprintf("r13=%016lx r14=%016lx r15=%016lx\n", r->r13, r->r14, r->r15);
    pprintf("cr2=%016lx cr3=%016lx\n", read_cr2(), read_cr3());
    if (r->vector == 14)
        pprintf("page fault: %s %s %s\n", (r->err & 1) ? "protection" : "not-present",
                (r->err & 2) ? "write" : "read", (r->err & 16) ? "exec" : "data");
    pprintf("verem:");
    const u64 *sp = (const u64 *)(uptr)r->rsp;
    for (int i = 0; i < 8; i++)
        pprintf(" %016lx", sp[i]);
    pprintf("\n*** megallas\n");
    if (console_ready())
        console_flush();
    store_dump(name, r);
    halt_forever();
}
