/* AO-OS kernel belepes. Sorrend: soros -> GDT/IDT/PIC/PIT -> PMM -> PAT/WC -> konzol
 * -> heap -> TSC -> billentyuzet -> PCI -> VFS (ramdisk, /tmp, /sys) -> taskok -> shell. */
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
#include "drv/ahci.h"
#include "drv/blk.h"
#include "drv/acpi.h"
#include "drv/e1000.h"
#include "drv/rtl8101.h"
#include "net/net.h"
#include "net/dhcp.h"
#include "fs/disk.h"
#include "fs/aofs.h"
#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"
#include "task/task.h"
#include "cap/cap.h"
#include "shell/shell.h"
#include "lib/fmt.h"
#include "lib/string.h"

#define AO_VERSION "0.1-phase2"

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

/* /sys/audit: az elutasitott capability-ellenorzesek */
static usize gen_audit(char *buf, usize cap)
{
    usize n = 0;
    for (u32 i = 0; i < audit_count(); i++) {
        const struct audit_entry *e = audit_get(i);
        n += snformat(buf + n, cap > n ? cap - n : 0, "%lu pid=%u %s %s %s\n", e->tick, e->pid,
                      cap_kind_name(e->kind), e->allowed ? "ok" : "deny", e->resource);
    }
    return n;
}

static usize gen_version(char *buf, usize cap)
{
    return snformat(buf, cap, "AO-OS %s\n", AO_VERSION);
}

static struct bootinfo *boot;
struct bootinfo *boot_info;

static void shell_thread(void *arg)
{
    (void)arg;
    shell_run(boot);
}

/* automatikus DHCP a hatterben, hogy a prompt ne varjon ra */
static void net_thread(void *arg)
{
    (void)arg;
    task_sleep_ms(200);
    int e = dhcp_run(8000);
    char ip[20];
    ip_format(net_cfg.ip, ip, sizeof ip);
    if (e == 0) kprintf("\n[net: dhcp ok, ip %s]\n", ip);
    else kprintf("\n[net: dhcp sikertelen, 'ip' paranccsal allithato]\n");
    console_flush();
}

void kmain(struct bootinfo *bi)
{
    boot = bi;
    boot_info = bi;
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

    /* nevter: / = ramdisk (AOFS v1, ro), /tmp = ramfs, /sys = ramfs + szintetikus fajlok */
    vfs_init();
    if (bi->ramdisk_size && aofs_mount(P2V(bi->ramdisk_paddr), bi->ramdisk_size)) {
        vfs_mount("/", &aofs1_ops, NULL, true);
        kprintf("ramdisk: AOFS v1, %u bejegyzes, %u KiB, csatolva: /\n", aofs_count(), bi->ramdisk_size / 1024);
    } else {
        kprintf("ramdisk: nincs\n");
    }
    void *tmpfs = ramfs_create();
    vfs_mount("/tmp", &ramfs_ops, tmpfs, false);
    void *sysfs = ramfs_create();
    vfs_mount("/sys", &ramfs_ops, sysfs, false);
    ramfs_add_generated(sysfs, "audit", gen_audit);
    ramfs_add_generated(sysfs, "version", gen_version);

    if (ahci_init()) {
        kprintf("ahci: port %u, %s, %lu MiB\n", ahci_port_index(), blk_model(), blk_sectors() / 2048);
        int e = disk_mount_root();
        if (e) kprintf("lemez: nincs AO-particio vagy AOFS v2 (install / mkfs)\n");
    } else {
        kprintf("ahci: nincs hasznalhato lemez (%s)\n", ahci_error());
    }
    kprintf("acpi: %s\n", acpi_init() ? "OK (poweroff elerheto)" : "nincs _S5");

    net_init();
    if (e1000_init() || rtl8101_init()) {
        const struct netdev *nd = net_dev();
        kprintf("net: %s  mac %02x:%02x:%02x:%02x:%02x:%02x\n", nd->name,
                nd->mac[0], nd->mac[1], nd->mac[2], nd->mac[3], nd->mac[4], nd->mac[5]);
    } else {
        kprintf("net: nincs tamogatott halozati kartya\n");
    }

    kprintf("pci: %u eszkoz   tsc: %lu MHz   kbd: i8042\n", pci_count(), tsc_hz() / 1000000);
    for (int i = 1; i < nstamp; i++)
        kprintf("  %-8s +%lu us\n", stamp_name[i], tsc_to_us(stamp[i] - stamp[i - 1]));
    kprintf("\n");
    console_flush();

    task_init();
    task_create_kernel("shell", shell_thread, NULL);
    if (net_dev())
        task_create_kernel("net", net_thread, NULL);
    task_idle_loop();
}
