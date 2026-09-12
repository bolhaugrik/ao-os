/* 8253/8254 PIT, 100 Hz tick. Ez a scheduler-tick es az idle-mero is. */
#include "pit.h"
#include "idt.h"
#include "pic.h"
#include "../arch/io.h"
#include "../task/task.h"
#include "../drv/kbd.h"
#include "../net/net.h"

static volatile u64 ticks;
static volatile u64 idle_ticks;
static volatile bool in_idle;

static void pit_irq(struct regs *r)
{
    (void)r;
    ticks++;
    if (in_idle)
        idle_ticks++;
    if (task_current()) {
        kbd_tick();
        net_poll();
        task_tick();
    }
}

void pit_init(void)
{
    u32 div = 1193182 / PIT_HZ;
    outb(0x43, 0x36);                   /* csatorna 0, lo/hi, mod 3 */
    outb(0x40, div & 0xFF);
    outb(0x40, (div >> 8) & 0xFF);
    irq_register(0, pit_irq);
    pic_unmask(0);
}

u64 pit_ticks(void) { return ticks; }
u64 pit_idle_ticks(void) { return idle_ticks; }

void idle_enter(void)
{
    in_idle = true;
    __asm__ volatile("sti; hlt; cli" ::: "memory");
    in_idle = false;
    sti();
}

void pit_sleep_ms(u32 ms)
{
    u64 end = ticks + (ms + 1000 / PIT_HZ - 1) / (1000 / PIT_HZ);
    while (ticks < end)
        idle_enter();
}
