/* 8259A PIC. Egy magon, APIC nelkul ez a legegyszerubb megszakitas-vezerlo. */
#include "pic.h"
#include "../arch/io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

void pic_init(void)
{
    outb(PIC1_CMD, 0x11); io_wait();   /* ICW1: init, ICW4 kovetkezik */
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 32);  io_wait();   /* ICW2: vektor-bazis */
    outb(PIC2_DATA, 40);  io_wait();
    outb(PIC1_DATA, 4);   io_wait();   /* ICW3: slave az IRQ2-n */
    outb(PIC2_DATA, 2);   io_wait();
    outb(PIC1_DATA, 0x01); io_wait();  /* ICW4: 8086 mod */
    outb(PIC2_DATA, 0x01); io_wait();
    outb(PIC1_DATA, 0xFB);             /* minden maszkolva, kiveve a kaszkad (IRQ2) */
    outb(PIC2_DATA, 0xFF);
}

void pic_unmask(u8 irq)
{
    u16 port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    u8 bit = irq & 7;
    outb(port, inb(port) & ~(1 << bit));
}

void pic_mask(u8 irq)
{
    u16 port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    u8 bit = irq & 7;
    outb(port, inb(port) | (1 << bit));
}

void pic_eoi(u8 irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

bool pic_irq_in_service(u8 irq)
{
    u16 port = irq < 8 ? PIC1_CMD : PIC2_CMD;
    outb(port, 0x0B);               /* OCW3: ISR olvasasa */
    return (inb(port) >> (irq & 7)) & 1;
}
