/* COM1 soros port. QEMU-ban ez a teszt-kimenet; a netbookon nincs port, a driver artalmatlan. */
#include "serial.h"
#include "../arch/io.h"

#define COM1 0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* megszakitasok ki */
    outb(COM1 + 3, 0x80);   /* DLAB */
    outb(COM1 + 0, 0x01);   /* 115200 */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xC7);   /* FIFO */
    outb(COM1 + 4, 0x03);   /* DTR | RTS */
}

void serial_putc(char c)
{
    if (c == '\n')
        serial_putc('\r');
    /* varakozas legfeljebb ~64k iteracioig, hogy port nelkul se akadjunk meg */
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
