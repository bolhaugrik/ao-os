/* AO-OS kernel belepes. Sorrend: soros -> GDT/IDT/PIC/PIT -> PMM -> PAT/WC -> konzol
 * -> heap -> TSC -> billentyuzet -> PCI -> ramdisk -> shell. */
#include "types.h"
#include "bootinfo.h"
#include "layout.h"
#include "arch/io.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/pic.h"
#include "cpu/pit.h"
#include "cpu/tsc.h"
#include "cpu/panic.h"
#include "drv/serial.h"
#include "drv/fb.h"
#include "drv/console.h"
#include "drv/kbd.h"
#include "drv/pci.h"
#include "fs/aofs.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"
#include "shell/shell.h"
#include "lib/fmt.h"

#define AO_VERSION "0.1-phase1"

static void serial_out(char c, void *ctx) { (void)ctx; serial_putc(c); }
static void sprintf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vformat(serial_out, NULL, fmt, ap);
    va_end(ap);
}

static u64 stamp[8];
static const char *stamp_name[8];
static int nstamp;
static void mark(const char *name) { if (nstamp < 8) { stamp[nstamp] = rdtsc(); stamp_name[nstamp++] = name; } }

void kmain(struct bootinfo *bi)
{
    serial_init();
    sprintf_("\nAO-OS v%s kernel\n", AO_VERSION);
    if (bi->magic != BOOTINFO_MAGIC) {
        sprintf_("bootinfo: rossz magic %08x\n", bi->magic);
        halt_forever();
    }
    mark("entry");

    gdt_init();
    idt_init();
    pic_init();
    pit_init();
    sti();
    mark("cpu");

    pmm_init(bi);
    vmm_init();
    fb_init(bi);
    vmm_set_wc(bi->fb_paddr, (u64)bi->fb_pitch * bi->fb_height);
    kheap_init();
    console_init();
    mark("mem+con");

    kprintf("AO-OS v%s  boot: %s\n", AO_VERSION, "OK");
    kprintf("fb: 0x%x %ux%u pitch %u (WC)  e820: %u bejegyzes  frame: %lu szabad / %lu\n",
            bi->fb_paddr, bi->fb_width, bi->fb_height, bi->fb_pitch, bi->e820_count,
            pmm_free_frames(), pmm_total_frames());
    console_flush();

    tsc_calibrate();
    kbd_init();
    pci_init();
    mark("drv");

    if (bi->ramdisk_size && aofs_mount(P2V(bi->ramdisk_paddr), bi->ramdisk_size))
        kprintf("ramdisk: AOFS v1, %u bejegyzes, %u KiB\n", aofs_count(), bi->ramdisk_size / 1024);
    else
        kprintf("ramdisk: nincs\n");
    kprintf("pci: %u eszkoz   tsc: %lu MHz   kbd: i8042\n", pci_count(), tsc_hz() / 1000000);
    for (int i = 1; i < nstamp; i++)
        kprintf("  %-8s +%lu us\n", stamp_name[i], tsc_to_us(stamp[i] - stamp[i - 1]));
    kprintf("\n");
    console_flush();

    shell_run(bi);
}
