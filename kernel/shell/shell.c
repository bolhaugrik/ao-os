/* AO-OS shell, Phase 1: kernel-modu, line editor elozmennyel, beepitett parancsok.
 * A parancsok egy "syscall alaku" belso API-t hivnak, hogy Phase 2-ben ring 3-ba
 * koltoztetheto legyen. */
#include "shell.h"
#include "layout.h"
#include "aox.h"
#include "../arch/io.h"
#include "../cpu/cpuid.h"
#include "../cpu/pit.h"
#include "../cpu/tsc.h"
#include "../cpu/panic.h"
#include "../drv/console.h"
#include "../drv/kbd.h"
#include "../drv/pci.h"
#include "../drv/fb.h"
#include "../fs/aofs.h"
#include "../mm/pmm.h"
#include "../mm/kheap.h"
#include "../lib/fmt.h"
#include "../lib/string.h"

#define LINE_MAX 256
#define HIST_MAX 64
#define ARGV_MAX 8

static const struct bootinfo *boot;
static char history[HIST_MAX][LINE_MAX];
static u32 hist_count, hist_pos;
static u64 boot_to_prompt_us, last_key_us;
static u32 cmd_count;

/* ---------------------------------------------------------------- kimenet */
static void con_out(char c, void *ctx)
{
    (void)ctx;
    console_putc(c);
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vformat(con_out, NULL, fmt, ap);
    va_end(ap);
    return 0;
}

/* ---------------------------------------------------------------- line editor */
/* A sort minden billentyu utan ujrarajzoljuk (prompt + puffer), majd a konzol-kurzort
 * a beszurasi pontra allitjuk. Egy sorra korlatozva (LINE_MAX < oszlopszam). */
static void redraw_line(const char *prompt, const char *line, u32 cur)
{
    kprintf("\r\x1b[K%s%s", prompt, line);
    console_set_col((u32)strlen(prompt) + cur);
    console_flush();
}

static void read_line(const char *prompt, char *line)
{
    u32 len = 0, cur = 0;
    int hist_nav = -1;
    line[0] = 0;
    kprintf("%s", prompt);
    console_flush();
    for (;;) {
        struct key_event ev;
        kbd_wait(&ev);
        u64 now = rdtsc();
        last_key_us = tsc_to_us(now - ev.tsc);
        u16 k = ev.code;
        if (k == '\n') {
            kprintf("\n");
            console_flush();
            break;
        } else if (k == '\b') {
            if (cur > 0) {
                memmove(line + cur - 1, line + cur, len - cur + 1);
                len--; cur--;
            }
        } else if (k == KEY_DEL) {
            if (cur < len) {
                memmove(line + cur, line + cur + 1, len - cur);
                len--;
            }
        } else if (k == KEY_LEFT) { if (cur) cur--; }
        else if (k == KEY_RIGHT) { if (cur < len) cur++; }
        else if (k == KEY_HOME) { cur = 0; }
        else if (k == KEY_END) { cur = len; }
        else if (k == KEY_UP || k == KEY_DOWN) {
            if (hist_count) {
                if (k == KEY_UP) hist_nav = (hist_nav < 0) ? (int)hist_count - 1 : (hist_nav > 0 ? hist_nav - 1 : 0);
                else hist_nav = (hist_nav < 0 || hist_nav >= (int)hist_count - 1) ? -1 : hist_nav + 1;
                if (hist_nav >= 0) strlcpy(line, history[hist_nav], LINE_MAX);
                else line[0] = 0;
                len = cur = (u32)strlen(line);
            }
        } else if (k == KEY_PGUP) { console_scroll_view((int)console_rows() / 2); console_flush(); continue; }
        else if (k == KEY_PGDN) { console_scroll_view(-(int)console_rows() / 2); console_flush(); continue; }
        else if (k == 3) {          /* Ctrl+C */
            kprintf("^C\n");
            line[0] = 0;
            len = cur = 0;
            kprintf("%s", prompt);
        } else if (k == 12) {       /* Ctrl+L */
            console_clear();
            kprintf("%s%s", prompt, line);
        } else if (k < 0x100 && k >= 32 && len < LINE_MAX - 1 &&
                   (u32)strlen(prompt) + len + 1 < console_cols()) {
            memmove(line + cur + 1, line + cur, len - cur + 1);
            line[cur] = (char)k;
            len++; cur++;
        } else {
            continue;
        }
        redraw_line(prompt, line, cur);
    }
}

static int split(char *line, char **argv)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < ARGV_MAX) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    return argc;
}

/* ---------------------------------------------------------------- parancsok */
static void cmd_help(void)
{
    kprintf("AO-OS parancsok:\n"
            "  help          ez a lista\n"
            "  mem           memoria: E820, frame-ek, heap\n"
            "  cpu           processzor: CPUID, TSC\n"
            "  disk          tarolo-vezerlok (PCI), AHCI portok\n"
            "  pci           minden PCI-eszkoz\n"
            "  ls [dir]      ramdisk tartalma\n"
            "  cat file      fajl kiirasa\n"
            "  run file      AOX program futtatasa\n"
            "  fb            framebuffer adatai\n"
            "  kbd [us|hu]   billentyuzetkiosztas\n"
            "  bench         merőszamok\n"
            "  uptime        ido a boot ota\n"
            "  echo ...      szoveg\n"
            "  clear         kepernyo torlese\n"
            "  crash [div|page|ud]  szandekos kivetel\n"
            "  reboot        ujrainditas\n"
            "  Shift/PgUp/PgDn gorgetes, Ctrl+C sor torlese, Ctrl+L clear\n");
}

static void cmd_mem(void)
{
    const struct e820_entry *e = P2V(boot->e820_paddr);
    u64 usable = 0;
    kprintf("E820 (%u bejegyzes):\n", boot->e820_count);
    for (u32 i = 0; i < boot->e820_count; i++) {
        const char *t = e[i].type == 1 ? "usable" : e[i].type == 3 ? "ACPI" : e[i].type == 4 ? "NVS" : "reserved";
        kprintf("  %012lx-%012lx %8lu KiB  %s\n", e[i].base, e[i].base + e[i].len - 1, e[i].len / KiB, t);
        if (e[i].type == 1) usable += e[i].len;
    }
    u64 tot = pmm_total_frames(), fr = pmm_free_frames();
    kprintf("hasznalhato: %lu MiB   frame: %lu osszes, %lu szabad, %lu foglalt (%lu MiB)\n",
            usable / MiB, tot, fr, tot - fr, (tot - fr) * PAGE_SIZE / MiB);
    kprintf("kernel: %u KiB @ 0x%x   ramdisk: %u KiB @ 0x%x\n",
            boot->kernel_size / 1024, boot->kernel_paddr, boot->ramdisk_size / 1024, boot->ramdisk_paddr);
    kprintf("heap: %lu KiB hasznalt / %lu KiB lefoglalt\n", kheap_used() / KiB, kheap_reserved() / KiB);
}

static void cmd_cpu(void)
{
    struct cpu_info ci;
    cpuid_read(&ci);
    kprintf("%s\n", ci.brand[0] ? ci.brand : "(nincs brand string)");
    kprintf("gyarto: %s  csalad: %u  modell: %u  stepping: %u  magok: %u\n",
            ci.vendor, ci.family, ci.model, ci.stepping, ci.cores);
    kprintf("tsc: %lu MHz%s  long mode: %s  nx: %s  syscall: %s\n",
            tsc_hz() / 1000000, ci.invariant_tsc ? " (invarians)" : "",
            ci.lm ? "igen" : "nem", ci.nx ? "igen" : "nem", ci.syscall_ok ? "igen" : "nem");
    kprintf("sse:%s sse2:%s sse3:%s ssse3:%s sse4.1:%s sse4.2:%s apic:%s\n",
            ci.sse ? "+" : "-", ci.sse2 ? "+" : "-", ci.sse3 ? "+" : "-", ci.ssse3 ? "+" : "-",
            ci.sse41 ? "+" : "-", ci.sse42 ? "+" : "-", ci.apic ? "+" : "-");
}

static void cmd_pci(void)
{
    for (u32 i = 0; i < pci_count(); i++) {
        const struct pci_dev *d = pci_get(i);
        kprintf("  %02x:%02x.%u  %04x:%04x  %02x.%02x.%02x  %s\n", d->bus, d->dev, d->fn,
                d->vendor, d->device, d->class_, d->subclass, d->progif,
                pci_class_name(d->class_, d->subclass));
    }
    kprintf("%u eszkoz\n", pci_count());
}

static void ahci_ports(const struct pci_dev *d)
{
    u64 abar = d->bar[5] & ~0xFULL;
    if (!abar) { kprintf("    ABAR nincs\n"); return; }
    volatile u32 *hba = P2V(abar);
    u32 cap = hba[0], pi = hba[3], vs = hba[4];
    kprintf("    ABAR=0x%lx  AHCI %u.%u  portok: %u, PI=0x%x, 64bit:%s\n", abar,
            vs >> 16, (vs >> 8) & 0xFF, (cap & 0x1F) + 1, pi, (cap >> 31) ? "igen" : "nem");
    for (u32 p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;
        volatile u32 *port = hba + 0x100 / 4 + p * 0x80 / 4;
        u32 ssts = port[0x28 / 4], sig = port[0x24 / 4];
        u32 det = ssts & 0xF, spd = (ssts >> 4) & 0xF;
        const char *what = "nincs eszkoz";
        if (det == 3) {
            what = sig == 0x00000101 ? "SATA lemez" : sig == 0xEB140101 ? "ATAPI" :
                   sig == 0xC33C0101 ? "enclosure" : sig == 0x96690101 ? "port multiplier" : "ismeretlen";
        }
        kprintf("    port %u: det=%u spd=%u sig=0x%08x  %s\n", p, det, spd, sig, what);
    }
}

static void cmd_disk(void)
{
    u32 n = 0;
    for (u32 i = 0; i < pci_count(); i++) {
        const struct pci_dev *d = pci_get(i);
        if (d->class_ != 0x01) continue;
        n++;
        kprintf("  %02x:%02x.%u  %04x:%04x  %s  irq=%u\n", d->bus, d->dev, d->fn, d->vendor, d->device,
                pci_class_name(d->class_, d->subclass), d->irq_line);
        if (d->subclass == 0x06)
            ahci_ports(d);
    }
    if (!n) kprintf("  nincs tarolo-vezerlo\n");
    kprintf("ramdisk (AOFS v1): %s, %u KiB, %u bejegyzes\n",
            aofs_mounted() ? "csatolva" : "nincs", aofs_image_size() / 1024, aofs_count());
}

static void cmd_ls(const char *dir)
{
    char prefix[96];
    const char *d = dir ? dir : "";
    while (*d == '/') d++;
    strlcpy(prefix, d, sizeof prefix);
    usize pl = strlen(prefix);
    if (pl && prefix[pl - 1] != '/') { prefix[pl++] = '/'; prefix[pl] = 0; }
    u32 n = 0;
    for (u32 i = 0; i < aofs_count(); i++) {
        const struct aofs_entry *e = aofs_entry(i);
        if (!str_starts(e->name, prefix)) continue;
        const char *rest = e->name + pl;
        if (!*rest) continue;
        bool nested = false;
        for (const char *p = rest; *p; p++) if (*p == '/') { nested = true; break; }
        if (nested) continue;
        if (e->type == AOFS_DIR) kprintf("  %-24s <dir>\n", rest);
        else kprintf("  %-24s %u B\n", rest, e->size);
        n++;
    }
    if (!n) kprintf("  (ures vagy nincs ilyen konyvtar)\n");
}

static void cmd_cat(const char *path)
{
    const struct aofs_entry *e = aofs_lookup(path);
    if (!e || e->type != AOFS_FILE) { kprintf("cat: nincs ilyen fajl: %s\n", path); return; }
    const char *p = aofs_data(e);
    for (u32 i = 0; i < e->size; i++)
        console_putc(p[i]);
    if (e->size && p[e->size - 1] != '\n')
        kprintf("\n");
}

/* AOX futtatas kernel modban. A program kimenetet minden hivas utan kirajzoljuk,
 * es varakozas elott is, kulonben csak a program vegen jelenne meg. */
static void api_puts(const char *s) { console_write(s); console_flush(); }
static int api_getc(void) { struct key_event ev; console_flush(); kbd_wait(&ev); return ev.code; }
static int api_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vformat(con_out, NULL, fmt, ap);
    va_end(ap);
    console_flush();
    return 0;
}
static const struct ao_api api = {
    .version = 1, .puts = api_puts, .getc = api_getc, .ticks = pit_ticks,
    .alloc = kmalloc, .free = kfree, .printf = api_printf,
};

static void cmd_run(int argc, char **argv)
{
    const struct aofs_entry *e = aofs_lookup(argv[1]);
    if (!e) {
        char alt[96];
        snformat(alt, sizeof alt, "bin/%s", argv[1]);
        e = aofs_lookup(alt);
        if (!e) { snformat(alt, sizeof alt, "bin/%s.aox", argv[1]); e = aofs_lookup(alt); }
    }
    if (!e || e->type != AOFS_FILE) { kprintf("run: nincs ilyen program: %s\n", argv[1]); return; }
    const struct aox_header *h = aofs_data(e);
    if (e->size < sizeof *h || h->magic != AOX_MAGIC) { kprintf("run: nem AOX fajl\n"); return; }
    if (h->entry >= h->load_size || h->load_size > 16 * MiB) { kprintf("run: hibas fejlec\n"); return; }
    /* az objcopy a zaro nullakat elhagyja: a fajl rovidebb lehet a load_size-nal */
    usize have = e->size < h->load_size ? e->size : h->load_size;
    usize total = h->load_size + h->bss_size;
    u8 *mem = kmalloc(total + 16);
    memcpy(mem, h, have);
    memset(mem + have, 0, total - have);
    aox_entry_fn entry = (aox_entry_fn)(mem + h->entry);
    u64 t0 = rdtsc();
    int rc = entry(&api, argc - 1, argv + 1);
    u64 dt = rdtsc() - t0;
    kfree(mem);
    kprintf("[%s: rc=%d, %lu us]\n", argv[1], rc, tsc_to_us(dt));
}

static void cmd_fb(void)
{
    kprintf("mod 0x%x  %ux%u  bpp %u  pitch %u  paddr 0x%x  rgb %u/%u/%u  konzol %ux%u\n",
            boot->vbe_mode, fb.width, fb.height, fb.bpp, fb.pitch, boot->fb_paddr,
            fb.rpos, fb.gpos, fb.bpos, console_cols(), console_rows());
}

static void cmd_bench(void)
{
    u64 t = pit_ticks(), idle = pit_idle_ticks();
    kprintf("boot -> prompt:     %lu ms (stage2 elejetol)\n", boot_to_prompt_us / 1000);
    kprintf("utolso billentyu:   %lu us (IRQ -> feldolgozas)\n", last_key_us);
    kprintf("idle:               %lu%% (%lu / %lu tick)\n", t ? idle * 100 / t : 0, idle, t);
    kprintf("kernel image:       %u KiB   heap: %lu KiB\n", boot->kernel_size / 1024, kheap_reserved() / KiB);
    u64 s = rdtsc();
    console_clear();
    for (int i = 0; i < 100; i++) kprintf("konzol-teszt sor %d: a gyors barna roka atugrik a lusta kutyan 0123456789\n", i);
    console_flush();
    u64 d = rdtsc() - s;
    kprintf("100 sor kiirasa:    %lu ms\n", tsc_to_ms(d));
    kprintf("parancsok eddig:    %u\n", cmd_count);
}

static void cmd_crash(const char *what)
{
    if (!what || strcmp(what, "div") == 0) {
        /* a fordito a C-szintu nullaval osztast kioptimalizalhatja (UB), ezert asm */
        __asm__ volatile("xor %%ecx, %%ecx; mov $1, %%eax; xor %%edx, %%edx; div %%ecx" ::: "eax", "ecx", "edx");
    } else if (strcmp(what, "page") == 0) {
        volatile u64 *p = (volatile u64 *)0xDEAD0000DEAD0000ULL;
        *p = 1;
    } else if (strcmp(what, "ud") == 0) {
        __asm__ volatile("ud2");
    } else {
        panic("crash: kezi panic (%s)", what);
    }
}

static void cmd_reboot(void)
{
    kprintf("ujrainditas...\n");
    console_flush();
    cli();
    for (u32 i = 0; i < 100000; i++)
        if (!(inb(0x64) & 2)) break;
    outb(0x64, 0xFE);
    /* ha az i8042 nem valaszol: tripla hiba */
    struct { u16 l; u64 b; } PACKED z = { 0, 0 };
    __asm__ volatile("lidt %0; int3" : : "m"(z));
    halt_forever();
}

static void execute(char *line)
{
    char *argv[ARGV_MAX];
    int argc = split(line, argv);
    if (!argc) return;
    cmd_count++;
    const char *c = argv[0];
    if (!strcmp(c, "help")) cmd_help();
    else if (!strcmp(c, "mem")) cmd_mem();
    else if (!strcmp(c, "cpu")) cmd_cpu();
    else if (!strcmp(c, "disk")) cmd_disk();
    else if (!strcmp(c, "pci")) cmd_pci();
    else if (!strcmp(c, "ls")) cmd_ls(argc > 1 ? argv[1] : NULL);
    else if (!strcmp(c, "cat")) { if (argc > 1) cmd_cat(argv[1]); else kprintf("cat: fajlnev kell\n"); }
    else if (!strcmp(c, "run")) { if (argc > 1) cmd_run(argc, argv); else kprintf("run: programnev kell\n"); }
    else if (!strcmp(c, "fb")) cmd_fb();
    else if (!strcmp(c, "kbd")) { if (argc > 1) kbd_set_layout(argv[1]); kprintf("kiosztas: %s\n", kbd_layout()); }
    else if (!strcmp(c, "bench")) cmd_bench();
    else if (!strcmp(c, "uptime")) kprintf("%lu.%02lu s\n", pit_ticks() / PIT_HZ, pit_ticks() % PIT_HZ);
    else if (!strcmp(c, "echo")) { for (int i = 1; i < argc; i++) kprintf("%s%s", argv[i], i + 1 < argc ? " " : ""); kprintf("\n"); }
    else if (!strcmp(c, "clear")) console_clear();
    else if (!strcmp(c, "crash")) cmd_crash(argc > 1 ? argv[1] : NULL);
    else if (!strcmp(c, "reboot")) cmd_reboot();
    else kprintf("ismeretlen parancs: %s (help)\n", c);
}

void shell_run(const struct bootinfo *bi)
{
    boot = bi;
    char line[LINE_MAX];
    boot_to_prompt_us = tsc_to_us(rdtsc() - bi->boot_tsc);
    console_set_color(CON_AMBER, CON_BLACK);
    kprintf("AO-OS v0.1  ");
    console_set_color(CON_DEFAULT_FG, CON_BLACK);
    kprintf("mem %luM  cpu %lu MHz  konzol %ux%u  kbd %s  boot %lu ms\n\n",
            pmm_total_frames() * PAGE_SIZE / MiB, tsc_hz() / 1000000, console_cols(), console_rows(),
            kbd_layout(), boot_to_prompt_us / 1000);
    for (;;) {
        read_line("AO> ", line);
        if (line[0]) {
            if (hist_count == 0 || strcmp(history[hist_count - 1], line) != 0) {
                if (hist_count == HIST_MAX) {
                    memmove(history[0], history[1], (HIST_MAX - 1) * LINE_MAX);
                    hist_count--;
                }
                strlcpy(history[hist_count++], line, LINE_MAX);
            }
            hist_pos = hist_count;
            execute(line);
        }
        console_flush();
    }
}
