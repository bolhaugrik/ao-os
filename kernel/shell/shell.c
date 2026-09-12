/* AO-OS shell, Phase 2: kernel-szalkent fut (gyoker-task, minden capability-vel),
 * a programokat ring 3-as taskkent inditja a VFS-bol, capability-keszlettel. */
#include "shell.h"
#include "layout.h"
#include "aox.h"
#include "syscall.h"
#include "../arch/io.h"
#include "../cpu/cpuid.h"
#include "../cpu/pit.h"
#include "../cpu/tsc.h"
#include "../cpu/panic.h"
#include "../drv/console.h"
#include "../drv/kbd.h"
#include "../drv/pci.h"
#include "../drv/fb.h"
#include "../fs/vfs.h"
#include "../fs/aofs.h"
#include "../mm/pmm.h"
#include "../mm/kheap.h"
#include "../task/task.h"
#include "../cap/cap.h"
#include "../lib/fmt.h"
#include "../lib/string.h"

#define LINE_MAX 256
#define HIST_MAX 64
#define ARGV_MAX 12

static const struct bootinfo *boot;
static char history[HIST_MAX][LINE_MAX];
static u32 hist_count;
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

static const char *errstr(int e)
{
    switch (e) {
    case E_OK: return "ok";
    case E_INVAL: return "ervenytelen";
    case E_NOENT: return "nincs ilyen";
    case E_CAP: return "nincs jogosultsag (E_CAP)";
    case E_NOMEM: return "nincs memoria";
    case E_BADF: return "rossz handle";
    case E_CHILD: return "nincs gyerek";
    case E_PIPE: return "pipe zarva";
    case E_NOSYS: return "nincs ilyen hivas";
    case E_TIMEOUT: return "hatarido/kill";
    case E_EXIST: return "mar letezik";
    case E_NOTDIR: return "nem konyvtar";
    case E_ISDIR: return "konyvtar";
    case E_LIMIT: return "limit";
    case E_IO: return "I/O hiba";
    case E_ROFS: return "csak olvashato";
    case E_BUSY: return "foglalt";
    default: return "hiba";
    }
}

/* ---------------------------------------------------------------- line editor */
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
        last_key_us = tsc_to_us(rdtsc() - ev.tsc);
        u16 k = ev.code;
        if (k == '\n') {
            kprintf("\n");
            console_flush();
            break;
        } else if (k == '\b') {
            if (cur > 0) { memmove(line + cur - 1, line + cur, len - cur + 1); len--; cur--; }
        } else if (k == KEY_DEL) {
            if (cur < len) { memmove(line + cur, line + cur + 1, len - cur); len--; }
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
        else if (k == 3) { kprintf("^C\n"); line[0] = 0; len = cur = 0; kprintf("%s", prompt); }
        else if (k == 12) { console_clear(); kprintf("%s%s", prompt, line); }
        else if (k < 0x100 && k >= 32 && len < LINE_MAX - 1 && (u32)strlen(prompt) + len + 1 < console_cols()) {
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

static const char *cwd(void) { return task_current()->cwd; }

static bool canon(const char *in, char *out)
{
    return vfs_canon(cwd(), in, out, VFS_PATH_MAX);
}

/* ---------------------------------------------------------------- parancsok */
static void cmd_help(void)
{
    kprintf("AO-OS parancsok:\n"
            "  help  /?  ?      ez a lista\n"
            "  mem cpu disk pci fb   rendszerinfo\n"
            "  ls [dir]  cat f  write f szoveg  rm f  mkdir d  cd d  pwd  mount\n"
            "  run prog [arg..]         AOX program ring 3-ban (gyoker-jogokkal)\n"
            "  spawn manifest prog [..] AOX program a manifest capability-keszletevel\n"
            "  ps  kill pid  caps [pid]  audit   taskok es jogosultsagok\n"
            "  kbd [us|hu]  bench  uptime  echo  clear  crash [div|page|ud]  reboot\n"
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
        if (det == 3)
            what = sig == 0x00000101 ? "SATA lemez" : sig == 0xEB140101 ? "ATAPI" :
                   sig == 0xC33C0101 ? "enclosure" : sig == 0x96690101 ? "port multiplier" : "ismeretlen";
        kprintf("    port %u: det=%u spd=%u sig=0x%08x  %s\n", p, det, spd, sig, what);
    }
}

/* AMD SB7x0/Hudson SATA (1022:7800): a BIOS IDE-modban adja at, de a vezerlo AHCI-kepes.
 * A Linux is igy kapcsolja at (quirk_amd_ide_mode). */
static bool amd_sata_to_ahci(struct pci_dev *d)
{
    if (d->vendor != 0x1022 || d->device != 0x7800 || d->subclass != 0x01)
        return false;
    u8 lock = pci_read8(d->bus, d->dev, d->fn, 0x40);
    pci_write8(d->bus, d->dev, d->fn, 0x40, lock | 1);
    pci_write8(d->bus, d->dev, d->fn, 0x09, 0x01);
    pci_write8(d->bus, d->dev, d->fn, 0x0A, 0x06);
    pci_write8(d->bus, d->dev, d->fn, 0x40, lock);
    pci_refresh(d);
    return d->subclass == 0x06;
}

static void cmd_disk(void)
{
    u32 n = 0;
    for (u32 i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get_mut(i);
        if (d->class_ != 0x01) continue;
        n++;
        kprintf("  %02x:%02x.%u  %04x:%04x  %s  prog-if=%02x  irq=%u\n", d->bus, d->dev, d->fn,
                d->vendor, d->device, pci_class_name(d->class_, d->subclass), d->progif, d->irq_line);
        if (d->subclass == 0x01 && amd_sata_to_ahci(d))
            kprintf("    AMD SATA: IDE-modbol AHCI-ra kapcsolva (BAR5=0x%x)\n", d->bar[5]);
        if (d->subclass == 0x06)
            ahci_ports(d);
    }
    if (!n) kprintf("  nincs tarolo-vezerlo\n");
    kprintf("ramdisk (AOFS v1): %s, %u KiB, %u bejegyzes\n",
            aofs_mounted() ? "csatolva" : "nincs", aofs_image_size() / 1024, aofs_count());
}

static void cmd_mount(void)
{
    for (u32 i = 0; ; i++) {
        const struct mount *m = vfs_mount_at(i);
        if (!m) break;
        kprintf("  %-8s %s%s\n", m->prefix, m->ops->name, m->ro ? " (ro)" : "");
    }
}

static void cmd_ls(const char *arg)
{
    char path[VFS_PATH_MAX];
    if (!canon(arg ? arg : ".", path)) { kprintf("ls: rossz utvonal\n"); return; }
    struct dirent *ents = kmalloc(64 * sizeof *ents);
    int n = vfs_list(path, ents, 64);
    if (n < 0) { kprintf("ls: %s: %s\n", path, errstr(n)); kfree(ents); return; }
    for (int i = 0; i < n; i++) {
        if (ents[i].type == 2) kprintf("  %-24s <dir>\n", ents[i].name);
        else kprintf("  %-24s %u B\n", ents[i].name, ents[i].size);
    }
    if (!n) kprintf("  (ures)\n");
    kfree(ents);
}

static void cmd_cat(const char *arg)
{
    char path[VFS_PATH_MAX];
    if (!canon(arg, path)) { kprintf("cat: rossz utvonal\n"); return; }
    void *buf;
    usize size;
    int e = vfs_read_all(path, &buf, &size);
    if (e) { kprintf("cat: %s: %s\n", path, errstr(e)); return; }
    const char *p = buf;
    for (usize i = 0; i < size; i++) console_putc(p[i]);
    if (size && p[size - 1] != '\n') kprintf("\n");
    kfree(buf);
}

static void cmd_write(int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    if (!canon(argv[1], path)) { kprintf("write: rossz utvonal\n"); return; }
    char text[LINE_MAX];
    usize n = 0;
    for (int i = 2; i < argc; i++) {
        n += snformat(text + n, sizeof text - n, "%s%s", argv[i], i + 1 < argc ? " " : "\n");
        if (n >= sizeof text) break;
    }
    int e = vfs_write_all(path, text, n);
    kprintf(e ? "write: %s: %s\n" : "%s: %lu bajt\n", path, e ? errstr(e) : (const char *)(uptr)n);
}

static void cmd_rm(const char *arg)
{
    char path[VFS_PATH_MAX];
    if (!canon(arg, path)) return;
    int e = vfs_unlink(path);
    if (e) kprintf("rm: %s: %s\n", path, errstr(e));
}

static void cmd_mkdir(const char *arg)
{
    char path[VFS_PATH_MAX];
    if (!canon(arg, path)) return;
    int e = vfs_mkdir(path);
    if (e) kprintf("mkdir: %s: %s\n", path, errstr(e));
}

static void cmd_cd(const char *arg)
{
    char path[VFS_PATH_MAX];
    if (!canon(arg ? arg : "/", path)) return;
    struct stat st;
    int e = vfs_stat(path, &st);
    if (e) { kprintf("cd: %s: %s\n", path, errstr(e)); return; }
    if (st.type != 2) { kprintf("cd: %s: nem konyvtar\n", path); return; }
    strlcpy(task_current()->cwd, path, sizeof task_current()->cwd);
}

/* program keresese: adott ut, vagy /bin/<nev>, /bin/<nev>.aox */
static bool find_prog(const char *name, char *path)
{
    struct stat st;
    if (canon(name, path) && vfs_stat(path, &st) == 0 && st.type == 1) return true;
    char alt[VFS_PATH_MAX];
    snformat(alt, sizeof alt, "/bin/%s", name);
    if (canon(alt, path) && vfs_stat(path, &st) == 0 && st.type == 1) return true;
    snformat(alt, sizeof alt, "/bin/%s.aox", name);
    if (canon(alt, path) && vfs_stat(path, &st) == 0 && st.type == 1) return true;
    return false;
}

static void run_with_caps(const struct capset *cs, int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    if (!find_prog(argv[0], path)) { kprintf("run: nincs ilyen program: %s\n", argv[0]); return; }
    void *img;
    usize size;
    int e = vfs_read_all(path, &img, &size);
    if (e) { kprintf("run: %s: %s\n", path, errstr(e)); return; }
    int err;
    console_flush();
    u64 t0 = rdtsc();
    struct task *t = task_create_user(argv[0], img, size, argc, argv, cs, task_current(), &err);
    kfree(img);
    if (!t) { kprintf("run: inditas sikertelen: %s\n", errstr(err)); return; }
    int status = 0;
    int pid = task_wait(t->id, &status);
    u64 dt = rdtsc() - t0;
    console_flush();
    kprintf("[%s: pid %d, rc=%d%s, %lu ms]\n", argv[0], pid, status,
            status == E_TIMEOUT ? " (hatarido/kill)" : "", tsc_to_ms(dt));
}

static void cmd_run(int argc, char **argv)
{
    run_with_caps(&task_current()->caps, argc - 1, argv + 1);
}

static void cmd_spawn(int argc, char **argv)
{
    if (argc < 3) { kprintf("spawn manifest prog [arg..]\n"); return; }
    char path[VFS_PATH_MAX];
    if (!canon(argv[1], path)) return;
    void *text;
    usize len;
    int e = vfs_read_all(path, &text, &len);
    if (e) { kprintf("spawn: %s: %s\n", path, errstr(e)); return; }
    struct capset cs;
    char err[64];
    bool ok = capset_parse(&cs, text, len, err, sizeof err);
    kfree(text);
    if (!ok) { kprintf("spawn: manifest hiba: %s\n", err); return; }
    if (!capset_subset(&cs, &task_current()->caps)) { kprintf("spawn: a manifest tobb jogot ker, mint a shell\n"); return; }
    kprintf("[agent %s: %u capability, mem %luK cpu %lums deadline %lums]\n", cs.name, cs.n,
            cs.limits.mem_bytes / 1024, cs.limits.cpu_ms, cs.limits.deadline_ms);
    run_with_caps(&cs, argc - 2, argv + 2);
}

static void cmd_ps(void)
{
    task_reap_orphans();
    kprintf("  pid  allapot   cpu(ms)  mem(KiB)  agent      nev\n");
    for (int i = 0; i < TASK_MAX; i++) {
        const struct task *t = task_at(i);
        if (!t || t->state == T_FREE) continue;
        const char *st = t->state == T_RUNNING ? "fut" : t->state == T_READY ? "kesz" :
                         t->state == T_BLOCKED ? "var" : "zombi";
        kprintf("  %3u  %-8s %8lu  %8lu  %-10s %s%s\n", t->id, st, t->cpu_ticks * 10, t->mem_used / 1024,
                t->caps.name, t->name, t->user ? "" : " (kernel)");
    }
}

static void cmd_caps(const char *arg)
{
    struct task *t = task_current();
    if (arg) {
        u32 pid = 0;
        for (const char *p = arg; *p >= '0' && *p <= '9'; p++) pid = pid * 10 + (u32)(*p - '0');
        t = task_get(pid);
        if (!t) { kprintf("caps: nincs ilyen task\n"); return; }
    }
    kprintf("agent %s (pid %u)\n", t->caps.name, t->id);
    for (u32 i = 0; i < t->caps.n; i++)
        kprintf("  %-9s %s\n", cap_kind_name(t->caps.caps[i].kind), t->caps.caps[i].pattern);
    if (t->caps.limits.mem_bytes) kprintf("  mem       %lu KiB\n", t->caps.limits.mem_bytes / 1024);
    if (t->caps.limits.cpu_ms) kprintf("  cpu       %lu ms\n", t->caps.limits.cpu_ms);
    if (t->caps.limits.deadline_ms) kprintf("  deadline  %lu ms\n", t->caps.limits.deadline_ms);
}

static void cmd_audit(void)
{
    u32 n = audit_count();
    if (!n) { kprintf("  (nincs elutasitas)\n"); return; }
    for (u32 i = 0; i < n; i++) {
        const struct audit_entry *e = audit_get(i);
        kprintf("  t=%lu.%02lus pid %u  %-9s %s  %s\n", e->tick / 100, e->tick % 100, e->pid,
                cap_kind_name(e->kind), e->allowed ? "OK" : "ELUTASITVA", e->resource);
    }
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
    kprintf("kernel image:       %u KiB   heap: %lu KiB   taskok: %u\n",
            boot->kernel_size / 1024, kheap_reserved() / KiB, task_count());
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
    vfs_sync();
    cli();
    for (u32 i = 0; i < 100000; i++)
        if (!(inb(0x64) & 2)) break;
    outb(0x64, 0xFE);
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
    if (!strcmp(c, "help") || !strcmp(c, "/?") || !strcmp(c, "?")) cmd_help();
    else if (!strcmp(c, "mem")) cmd_mem();
    else if (!strcmp(c, "cpu")) cmd_cpu();
    else if (!strcmp(c, "disk")) cmd_disk();
    else if (!strcmp(c, "pci")) cmd_pci();
    else if (!strcmp(c, "mount")) cmd_mount();
    else if (!strcmp(c, "ls")) cmd_ls(argc > 1 ? argv[1] : NULL);
    else if (!strcmp(c, "cat")) { if (argc > 1) cmd_cat(argv[1]); else kprintf("cat: fajlnev kell\n"); }
    else if (!strcmp(c, "write")) { if (argc > 2) cmd_write(argc, argv); else kprintf("write fajl szoveg...\n"); }
    else if (!strcmp(c, "rm")) { if (argc > 1) cmd_rm(argv[1]); }
    else if (!strcmp(c, "mkdir")) { if (argc > 1) cmd_mkdir(argv[1]); }
    else if (!strcmp(c, "cd")) cmd_cd(argc > 1 ? argv[1] : NULL);
    else if (!strcmp(c, "pwd")) kprintf("%s\n", cwd());
    else if (!strcmp(c, "run")) { if (argc > 1) cmd_run(argc, argv); else kprintf("run: programnev kell\n"); }
    else if (!strcmp(c, "spawn")) cmd_spawn(argc, argv);
    else if (!strcmp(c, "ps")) cmd_ps();
    else if (!strcmp(c, "kill")) {
        u32 pid = 0;
        if (argc > 1) for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) pid = pid * 10 + (u32)(*p - '0');
        kprintf(task_kill(pid) ? "kill: pid %u megjelolve\n" : "kill: nincs ilyen user-task (%u)\n", pid);
    }
    else if (!strcmp(c, "caps")) cmd_caps(argc > 1 ? argv[1] : NULL);
    else if (!strcmp(c, "audit")) cmd_audit();
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
        char prompt[VFS_PATH_MAX + 8];
        if (strcmp(cwd(), "/") == 0) strcpy(prompt, "AO> ");
        else snformat(prompt, sizeof prompt, "AO %s> ", cwd());
        read_line(prompt, line);
        if (line[0]) {
            if (hist_count == 0 || strcmp(history[hist_count - 1], line) != 0) {
                if (hist_count == HIST_MAX) {
                    memmove(history[0], history[1], (HIST_MAX - 1) * LINE_MAX);
                    hist_count--;
                }
                strlcpy(history[hist_count++], line, LINE_MAX);
            }
            execute(line);
        }
        console_flush();
    }
}
