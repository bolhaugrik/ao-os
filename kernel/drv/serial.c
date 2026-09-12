/* COM1 soros port. QEMU-ban ez a teszt-kimenet es -bemenet; a netbookon nincs port,
 * a driver artalmatlan (a nem letezo port 0xFF-et ad vissza). */
#include "serial.h"
#include "../arch/io.h"

#define COM1 0x3F8
static bool present;

void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* megszakitasok ki */
    outb(COM1 + 3, 0x80);   /* DLAB */
    outb(COM1 + 0, 0x01);   /* 115200 */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xC7);   /* FIFO */
    outb(COM1 + 4, 0x03);   /* DTR | RTS */
    /* jelenlet: a scratch-regiszter irhato-olvashato, ha van UART */
    outb(COM1 + 7, 0x5A);
    present = inb(COM1 + 7) == 0x5A;
}

void serial_putc(char c)
{
    if (!present)
        return;
    if (c == '\n')
        serial_putc('\r');
    for (u32 i = 0; i < 65536; i++)
        if (inb(COM1 + 5) & 0x20)
            break;
    outb(COM1, (u8)c);
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

bool serial_has_input(void)
{
    return present && (inb(COM1 + 5) & 1);
}

u8 serial_getc(void)
{
    return inb(COM1);
}
