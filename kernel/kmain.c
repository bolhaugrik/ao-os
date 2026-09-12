/* AO-OS kernel, Phase 0: bootinfo kiirasa a soros portra, framebuffer-teszt, megallas. */
#include "types.h"
#include "bootinfo.h"
#include "layout.h"
#include "arch/io.h"
#include "drv/serial.h"
#include "drv/fb.h"
#include "lib/fmt.h"

#define AO_VERSION "0.1-phase0"

static void serial_out(char c, void *ctx)
{
    (void)ctx;
    serial_putc(c);
}

static void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vformat(serial_out, NULL, fmt, ap);
    va_end(ap);
}

static const char *e820_type_name(u32 t)
{
    switch (t) {
    case E820_USABLE:   return "usable";
    case E820_RESERVED: return "reserved";
    case E820_ACPI:     return "ACPI data";
    case E820_NVS:      return "ACPI NVS";
    default:            return "other";
    }
}

/* Phase 0 mini-font: csak az "AO-OS" es a '>' glyphjei. A teljes font Phase 1. */
static const u8 glyph_A[16] = { 0x00,0x00,0x18,0x3C,0x66,0x66,0x66,0x7E,0x7E,0x66,0x66,0x66,0x66,0x00,0x00,0x00 };
static const u8 glyph_O[16] = { 0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00 };
static const u8 glyph_S[16] = { 0x00,0x00,0x3C,0x66,0x60,0x60,0x3C,0x06,0x06,0x06,0x66,0x66,0x3C,0x00,0x00,0x00 };
static const u8 glyph_dash[16] = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
static const u8 glyph_gt[16] = { 0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00 };

static void fb_test(void)
{
    const u32 bg    = fb_rgb(0x0C, 0x11, 0x16);
    const u32 amber = fb_rgb(0xE8, 0xB6, 0x4A);
    const u32 dim   = fb_rgb(0x7C, 0x8B, 0x99);

    fb_fill(bg);

    /* "AO-OS" 4x nagyitasban, majd "AO>" alatta 2x-ben */
    const u8 *word[5] = { glyph_A, glyph_O, glyph_dash, glyph_O, glyph_S };
    u32 x = 40, y = 40, scale = 4;
    for (int i = 0; i < 5; i++, x += 9 * scale)
        fb_glyph(x, y, word[i], scale, amber);

    const u8 *prompt[3] = { glyph_A, glyph_O, glyph_gt };
    x = 40;
    y = 40 + 16 * scale + 24;
    scale = 2;
    for (int i = 0; i < 3; i++, x += 9 * scale)
        fb_glyph(x, y, prompt[i], scale, dim);
    fb_rect(x, y, 8 * scale, 16 * scale, amber);   /* kurzor */

    /* szinsav a jobb also sarokban: R G B feher, a csatorna-sorrend ellenorzesere */
    u32 bar_w = 40, bar_h = 12;
    u32 bx = fb.width - 4 * bar_w - 16, by = fb.height - bar_h - 16;
    fb_rect(bx + 0 * bar_w, by, bar_w, bar_h, fb_rgb(255, 0, 0));
    fb_rect(bx + 1 * bar_w, by, bar_w, bar_h, fb_rgb(0, 255, 0));
    fb_rect(bx + 2 * bar_w, by, bar_w, bar_h, fb_rgb(0, 0, 255));
    fb_rect(bx + 3 * bar_w, by, bar_w, bar_h, fb_rgb(255, 255, 255));

    /* boot-fazis jelzo a jobb felso sarokban (Phase 0 = 1 negyzet) */
    fb_rect(fb.width - 24, 8, 16, 16, amber);
}

void kmain(struct bootinfo *bi)
{
    serial_init();
    kprintf("\nAO-OS v%s kernel\n", AO_VERSION);

    if (bi->magic != BOOTINFO_MAGIC) {
        kprintf("bootinfo: rossz magic %08x\n", bi->magic);
        halt_forever();
    }

    u64 now = rdtsc();
    kprintf("boot: drive=0x%02x tsc_boot=%lu tsc_now=%lu delta=%lu\n",
            bi->boot_drive, bi->boot_tsc, now, now - bi->boot_tsc);
    kprintf("kernel: paddr=0x%x size=%u  ramdisk: paddr=0x%x size=%u\n",
            bi->kernel_paddr, bi->kernel_size, bi->ramdisk_paddr, bi->ramdisk_size);
    kprintf("fb: mode=0x%x %ux%u bpp=%u pitch=%u paddr=0x%x rgb_pos=%u/%u/%u\n",
            bi->vbe_mode, bi->fb_width, bi->fb_height, bi->fb_bpp, bi->fb_pitch,
            bi->fb_paddr, bi->fb_rpos, bi->fb_gpos, bi->fb_bpos);

    const struct e820_entry *e = P2V(bi->e820_paddr);
    u64 usable = 0;
    kprintf("e820: %u bejegyzes\n", bi->e820_count);
    for (u32 i = 0; i < bi->e820_count; i++) {
        kprintf("  %016lx - %016lx  %s\n", e[i].base, e[i].base + e[i].len - 1,
                e820_type_name(e[i].type));
        if (e[i].type == E820_USABLE)
            usable += e[i].len;
    }
    kprintf("mem: hasznalhato %lu KiB (%lu MiB)\n", usable / KiB, usable / MiB);

    fb_init(bi);
    fb_test();
    kprintf("fb: teszt kirajzolva\n");
    kprintf("phase0: kesz, hlt\n");

    halt_forever();
}
