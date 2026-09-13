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
#include "../fs/disk.h"
#include "../drv/blk.h"
#include "../drv/ahci.h"
#include "../drv/acpi.h"
#include "../drv/rtc.h"
#include "version.h"
#include "../net/net.h"
#include "../net/tcp.h"
#include "../net/dhcp.h"
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
static bool quiet_run;                  /* run_with_caps: nincs "[prog: pid, rc]" sor (shot/copy/paste) */
static int  bridge_state;               /* 0 meg nem ellenorzott, 1 elerheto, -1 nem valaszol */
static volatile bool net_busy;          /* a boot-kori DHCP + PING fut: az agent megvarja */

void shell_net_boot(bool busy) { net_busy = busy; }
static u64  cur_prompt_seq;             /* az aktualis parancs sora a konzolon */
static u64  prev_out_start, prev_out_end;   /* az elozo parancs kimenetenek sorai [start, end) */
static char paste_buf[LINE_MAX];
static bool have_paste;

static void execute(char *line);

/* ---------------------------------------------------------------- parancstabla */
struct cmd { const char *names; const char *args; const char *desc; u8 group; };
static const char *const group_names[] = {
    "Rendszer", "Fajlok", "Programok es agentek", "AI, hid, PC", "Halozat", "Lemez", "Egyeb"
};
static const struct cmd cmds[] = {
    { "help ? /?",       "[PARANCS]",            "parancsok listaja; help PARANCS a reszletekhez", 0 },
    { "status",          "",                     "allapot: gep, ido, lemez, halozat, hid", 0 },
    { "time",            "[set OO:PP[:MM]]",     "pontos ido a CMOS orabol; beallitas", 0 },
    { "date",            "[set EEEE-HH-NN]",     "datum es nap neve; beallitas", 0 },
    { "mem cpu pci fb",  "",                     "memoria-terkep, processzor, PCI-eszkozok, kepernyo", 0 },
    { "uptime bench",    "",                     "futasido; boot- es konzol-sebesseg", 0 },
    { "kbd",             "[us|hu]",              "billentyuzet-kiosztas", 0 },
    { "clear",           "",                     "kepernyo torlese (Ctrl+L is)", 0 },
    { "ls",              "[KONYVTAR]",           "konyvtar tartalma", 1 },
    { "cat",             "FAJL",                 "fajl kiirasa", 1 },
    { "write",           "FAJL SZOVEG",          "fajl irasa (felulir, egy sor)", 1 },
    { "append",          "FAJL SZOVEG",          "sor hozzafuzese a fajl vegehez", 1 },
    { "rm mkdir",        "UTVONAL",              "torles; konyvtar letrehozasa", 1 },
    { "cd pwd",          "[KONYVTAR]",           "munkakonyvtar valtasa; kiirasa", 1 },
    { "mount",           "",                     "csatolt fajlrendszerek", 1 },
    { "run",             "PROG [ARG..]",         "AOX program ring 3-ban, a shell jogaival", 2 },
    { "spawn",           "MANIFEST PROG [ARG..]","program a manifest capability-keszletevel", 2 },
    { "ps kill",         "[PID]",                "taskok; task leallitasa", 2 },
    { "caps audit",      "[PID]",                "capability-k; elutasitott hozzaferesek naploja", 2 },
    { "ai",              "KERDES",               "egyszeri kerdes az AI-nak (chat agent)", 3 },
    { "agent",           "NEV FELADAT",          "agent a NEV.cap manifesttel (/state/agents, /etc/agents)", 3 },
    { "projector",       "CIM | KERDES",         "weboldal vagy kereses lenyomata a teljes kepernyon (--dump: szoveg)", 3 },
    { "shot",            "[NEV]",                "a kepernyo szovege a PC-re (a hid shots/ mappajaba)", 3 },
    { "copy",            "[N]",                  "az utolso parancs kimenete (vagy N sor) a PC vagolapjara", 3 },
    { "paste",           "[FAJL]",               "a PC vagolapja a parancssorba vagy fajlba", 3 },
    { "fetch",           "NEV [CEL]",            "fajl a PC share/ mappajabol (alap: /state/inbox/NEV)", 3 },
    { "net dhcp",        "",                     "halozati allapot; cim kerese DHCP-vel", 4 },
    { "ip",              "CIM MASZK [ATJARO]",   "statikus cim", 4 },
    { "ping nc",         "CIM [PORT [SZOVEG]]",  "ICMP ping; TCP proba", 4 },
    { "disk",            "",                     "tarolo-vezerlok, lemez, particio", 5 },
    { "install update",  "",                     "telepites a belso lemezre; frissites (/state marad)", 5 },
    { "mkfs sync",       "",                     "particio formazasa; irasok kiirasa", 5 },
    { "lastpanic",       "[clear]",              "az utolso panic a lemezrol", 5 },
    { "2048",            "",                     "a jatek: nyilak, r uj jatek, q kilep (legjobb: /state/games)", 6 },
    { "doom",            "[ARG..]",              "Doom (a /state/games/doom1.wad kell: fetch doom1.wad /state/games)", 6 },
    { "echo",            "SZOVEG",               "szoveg kiirasa", 6 },
    { "crash",           "[div|page|ud]",        "szandekos kernel-kivetel (teszt)", 6 },
    { "reboot poweroff", "",                     "ujrainditas; kikapcsolas (ACPI)", 6 },
};
#define NCMDS (sizeof cmds / sizeof cmds[0])

/* a parancsnevek kulon-kulon (Tab-kiegeszites) */
static char cmd_names[64][16];
static int n_cmd_names;

static void build_cmd_names(void)
{
    for (usize i = 0; i < NCMDS && n_cmd_names < 64; i++) {
        const char *p = cmds[i].names;
        while (*p && n_cmd_names < 64) {
            int k = 0;
            while (*p && *p != ' ' && k < 15) cmd_names[n_cmd_names][k++] = *p++;
            cmd_names[n_cmd_names++][k] = 0;
            while (*p == ' ') p++;
        }
    }
}

static bool starts_with(const char *s, const char *prefix, u32 n)
{
    for (u32 i = 0; i < n; i++) if (s[i] != prefix[i] || !s[i]) return false;
    return true;
}

/* a "nev" benne van-e a szokozzel elvalasztott listaban */
static bool names_contain(const char *names, const char *name)
{
    usize l = strlen(name);
    const char *p = names;
    while (*p) {
        if (starts_with(p, name, (u32)l) && (p[l] == ' ' || p[l] == 0)) return true;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }
    return false;
}

static void label(const char *s)
{
    console_set_color(CON_AMBER, CON_BLACK);
    kprintf("%s", s);
    console_set_color(CON_DEFAULT_FG, CON_BLACK);
}

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
    case E_FAULT: return "kivetel (E_FAULT)";
    default: return "hiba";
    }
}

/* ---------------------------------------------------------------- line editor */
/* A sor UTF-8 bajtokat tartalmaz; a kurzor bajt-indexu, a megjelenitett oszlop a nem-folytato
 * bajtok szama. A sort minden billentyu utan ujrarajzoljuk (prompt + puffer). */
static inline bool is_cont(char c) { return ((u8)c & 0xC0) == 0x80; }

static u32 display_width(const char *s, u32 nbytes)
{
    u32 w = 0;
    for (u32 i = 0; i < nbytes; i++) if (!is_cont(s[i])) w++;
    return w;
}

static void redraw_line(const char *prompt, const char *line, u32 cur)
{
    kprintf("\r\x1b[K%s%s", prompt, line);
    console_set_col((u32)strlen(prompt) + display_width(line, cur));
    console_flush();
}

static const char *cwd(void);
static bool canon(const char *in, char *out);

/* Tab: az elso szo parancsnev, a tobbi utvonal. Egy talalat: beirjuk (+ szokoz, konyvtarnal '/'),
 * tobb talalat: a kozos elotagot irjuk be es kilistazzuk oket. */
static void complete(char *line, u32 *len, u32 *cur, const char *prompt)
{
    static char cands[32][64];
    u32 ws = *cur;
    while (ws > 0 && line[ws - 1] != ' ') ws--;
    bool first = true;
    for (u32 i = 0; i < ws; i++) if (line[i] != ' ') { first = false; break; }
    char word[LINE_MAX];
    u32 wl = *cur - ws;
    memcpy(word, line + ws, wl);
    word[wl] = 0;
    int nc = 0;
    u32 bl = wl;                                    /* a kiegeszitendo resz hossza */
    if (first) {
        for (int i = 0; i < n_cmd_names && nc < 32; i++)
            if (starts_with(cmd_names[i], word, wl)) strlcpy(cands[nc++], cmd_names[i], 64);
    } else {
        char dir[LINE_MAX];
        const char *base = word;
        int slash = -1;
        for (u32 i = 0; i < wl; i++) if (word[i] == '/') slash = (int)i;
        if (slash >= 0) {
            memcpy(dir, word, (usize)slash);
            dir[slash] = 0;
            if (slash == 0) strcpy(dir, "/");
            base = word + slash + 1;
            bl = wl - (u32)slash - 1;
        } else {
            strcpy(dir, ".");
        }
        char path[VFS_PATH_MAX];
        if (!canon(dir, path)) return;
        struct dirent *ents = kmalloc(64 * sizeof *ents);
        int n = vfs_list(path, ents, 64);
        for (int i = 0; i < n && nc < 32; i++) {
            if (!starts_with(ents[i].name, base, bl)) continue;
            snformat(cands[nc++], 64, "%s%s", ents[i].name, ents[i].type == 2 ? "/" : "");
        }
        kfree(ents);
    }
    if (!nc) return;
    u32 lcp = (u32)strlen(cands[0]);
    for (int i = 1; i < nc; i++) {
        u32 k = 0;
        while (k < lcp && cands[i][k] == cands[0][k]) k++;
        lcp = k;
    }
    u32 il = lcp - bl;
    bool space = nc == 1 && cands[0][lcp - 1] != '/';
    u32 add = il + (space ? 1 : 0);
    if (*len + add < LINE_MAX - 4) {
        memmove(line + *cur + add, line + *cur, *len - *cur + 1);
        memcpy(line + *cur, cands[0] + bl, il);
        if (space) line[*cur + il] = ' ';
        *len += add;
        *cur += add;
    }
    if (nc > 1) {
        kprintf("\n");
        for (int i = 0; i < nc; i++) kprintf("%-20s%s", cands[i], (i % 8 == 7 || i == nc - 1) ? "\n" : "");
        kprintf("%s", prompt);
    }
}

static void read_line_init(const char *prompt, char *line, const char *init)
{
    u32 len = 0, cur = 0;
    int hist_nav = -1;
    line[0] = 0;
    if (init) {
        strlcpy(line, init, LINE_MAX);
        len = cur = (u32)strlen(line);
    }
    kprintf("%s%s", prompt, line);
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
            if (cur > 0) {
                u32 s = cur - 1;
                while (s > 0 && is_cont(line[s])) s--;
                memmove(line + s, line + cur, len - cur + 1);
                len -= cur - s; cur = s;
            }
        } else if (k == KEY_DEL) {
            if (cur < len) {
                u32 e = cur + 1;
                while (e < len && is_cont(line[e])) e++;
                memmove(line + cur, line + e, len - e + 1);
                len -= e - cur;
            }
        } else if (k == KEY_LEFT) { if (cur) { cur--; while (cur > 0 && is_cont(line[cur])) cur--; } }
        else if (k == KEY_RIGHT) { if (cur < len) { cur++; while (cur < len && is_cont(line[cur])) cur++; } }
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
        else if (k == '\t') { complete(line, &len, &cur, prompt); }
        else if (!KEY_IS_SPECIAL(k) && k >= 32 && len < LINE_MAX - 4 &&
                 (u32)strlen(prompt) + display_width(line, len) + 1 < console_cols()) {
            char enc[4];
            u32 n = utf8_encode(k, enc);
            memmove(line + cur + n, line + cur, len - cur + 1);
            memcpy(line + cur, enc, n);
            len += n; cur += n;
        } else {
            continue;
        }
        redraw_line(prompt, line, cur);
    }
}

static void read_line(const char *prompt, char *line) { read_line_init(prompt, line, NULL); }

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
static void cmd_help(const char *what)
{
    if (what) {
        for (usize i = 0; i < NCMDS; i++) {
            if (!names_contain(cmds[i].names, what)) continue;
            label("  ");
            label(cmds[i].names);
            kprintf(" %s\n    %s\n", cmds[i].args, cmds[i].desc);
            return;
        }
        kprintf("help: nincs ilyen parancs: %s\n", what);
        return;
    }
    label("AO-OS " AO_VERSION " parancsok");
    kprintf("   (help PARANCS = reszletek, Tab = kiegeszites)\n");
    for (u8 g = 0; g < sizeof group_names / sizeof group_names[0]; g++) {
        label(group_names[g]);
        kprintf("\n");
        for (usize i = 0; i < NCMDS; i++) {
            if (cmds[i].group != g) continue;
            char left[48];
            snformat(left, sizeof left, "%s %s", cmds[i].names, cmds[i].args);
            kprintf("  %-38s%s\n", left, cmds[i].desc);
        }
    }
    console_set_color(CON_BBLACK, CON_BLACK);
    kprintf("Billentyuk: Tab kiegeszites, Fel/Le elozmenyek, PgUp/PgDn gorgetes, Ctrl+C sor torlese, Ctrl+L clear.\n"
            "Indito szkript: /state/rc (vagy /etc/rc), soronkent parancsok, '#' megjegyzes.\n");
    console_set_color(CON_DEFAULT_FG, CON_BLACK);
}

/* ---------------------------------------------------------------- ido */
static bool parse_fields(const char *s, char sep, u32 *v, int n, int min_fields)
{
    int f = 0;
    for (int i = 0; i < n; i++) v[i] = 0;
    while (*s && f < n) {
        if (*s < '0' || *s > '9') return false;
        while (*s >= '0' && *s <= '9') v[f] = v[f] * 10 + (u32)(*s++ - '0');
        f++;
        if (*s == sep) s++;
        else if (*s) return false;
    }
    return f >= min_fields && !*s;
}

static void cmd_time(int argc, char **argv)
{
    struct rtc_time t;
    if (argc >= 3 && !strcmp(argv[1], "set")) {
        u32 v[3];
        if (!parse_fields(argv[2], ':', v, 3, 2) || v[0] > 23 || v[1] > 59 || v[2] > 59) { kprintf("time set OO:PP[:MM]\n"); return; }
        rtc_read(&t);
        t.hour = (u8)v[0]; t.min = (u8)v[1]; t.sec = (u8)v[2];
        rtc_write(&t);
    }
    if (!rtc_read(&t)) { kprintf("time: az RTC nem olvashato\n"); return; }
    kprintf("%02lu:%02lu:%02lu\n", (u64)t.hour, (u64)t.min, (u64)t.sec);
}

static void cmd_date(int argc, char **argv)
{
    struct rtc_time t;
    if (argc >= 3 && !strcmp(argv[1], "set")) {
        u32 v[3];
        if (!parse_fields(argv[2], '-', v, 3, 3) || v[0] < 2000 || v[0] > 2099 || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31) {
            kprintf("date set EEEE-HH-NN\n");
            return;
        }
        rtc_read(&t);
        t.year = (u16)v[0]; t.mon = (u8)v[1]; t.day = (u8)v[2];
        rtc_write(&t);
    }
    if (!rtc_read(&t)) { kprintf("date: az RTC nem olvashato\n"); return; }
    kprintf("%04lu-%02lu-%02lu %s\n", (u64)t.year, (u64)t.mon, (u64)t.day, rtc_weekday_name(rtc_weekday(&t)));
}

/* ---------------------------------------------------------------- allapot, hid */
static bool have_psk_file(void)
{
    struct stat st;
    return vfs_stat("/state/ai/psk", &st) == 0 && st.size >= 64;
}

static bool bridge_addr(char *out, usize cap)
{
    void *b;
    usize n;
    if (vfs_read_all("/state/ai/bridge", &b, &n) && vfs_read_all("/etc/ai/bridge", &b, &n)) return false;
    usize l = 0;
    const char *p = b;
    while (l < n && l + 1 < cap && p[l] != '\n' && p[l] != '\r') { out[l] = p[l]; l++; }
    out[l] = 0;
    kfree(b);
    return l > 0;
}

void shell_probe_bridge(void)
{
    char addr[64];
    if (!bridge_addr(addr, sizeof addr)) { net_busy = false; return; }
    char host[64];
    u32 port = 0;
    usize i = 0;
    while (addr[i] && addr[i] != ':' && i < sizeof host - 1) { host[i] = addr[i]; i++; }
    host[i] = 0;
    if (addr[i] == ':') for (const char *p = addr + i + 1; *p >= '0' && *p <= '9'; p++) port = port * 10 + (u32)(*p - '0');
    u32 ip;
    bool ok = false;
    if (ip_parse(host, &ip) && port) {
        int s = tcp_connect(ip, (u16)port, 3000);
        if (s >= 0) {
            static const u8 ping[12] = { 'A', 'O', 'P', '1', 10, 0, 0, 0, 0, 0, 0, 0 };   /* PING, len 0 */
            if (tcp_send(s, ping, 12) == 12) {
                u8 r[12];
                usize got = 0;
                while (got < 12) {
                    isize n = tcp_recv(s, r + got, 12 - got, 3000);
                    if (n <= 0) break;
                    got += (usize)n;
                }
                ok = got == 12 && r[4] == 11;       /* PONG */
            }
            tcp_close(s);
        }
    }
    bridge_state = ok ? 1 : -1;
    net_busy = false;
    kprintf("[hid: %s %s, PSK %s]\n", addr, ok ? "elerheto" : "nem valaszol", have_psk_file() ? "van" : "nincs");
    console_flush();
}

static void print_status(void)
{
    struct cpu_info ci;
    cpuid_read(&ci);
    label("  gep      ");
    kprintf("%s, %lu MHz, %lu MiB, %ux%u (konzol %ux%u)\n", ci.brand[0] ? ci.brand : ci.vendor,
            tsc_hz() / 1000000, pmm_total_frames() * PAGE_SIZE / MiB, boot->fb_width, boot->fb_height,
            console_cols(), console_rows());
    struct rtc_time t;
    label("  ido      ");
    if (rtc_read(&t))
        kprintf("%04lu-%02lu-%02lu %s %02lu:%02lu   ", (u64)t.year, (u64)t.mon, (u64)t.day,
                rtc_weekday_name(rtc_weekday(&t)), (u64)t.hour, (u64)t.min);
    else
        kprintf("(RTC nem olvashato)   ");
    kprintf("futasido %lu s   boot %lu ms   kiosztas %s\n", pit_ticks() / PIT_HZ, boot_to_prompt_us / 1000, kbd_layout());
    label("  lemez    ");
    if (blk_present()) {
        const char *rootfs = "?";
        bool ro = false;
        for (u32 i = 0; ; i++) {
            const struct mount *m = vfs_mount_at(i);
            if (!m) break;
            if (!strcmp(m->prefix, "/")) { rootfs = m->ops->name; ro = m->ro; }
        }
        kprintf("%s, %lu GiB, gyoker: %s%s", blk_model(), blk_sectors() / 2097152, rootfs, ro ? " (ro)" : "");
    } else {
        kprintf("nincs (ramdiskrol fut)");
    }
    kprintf("   ramdisk %u KiB\n", boot->ramdisk_size / 1024);
    label("  halozat  ");
    struct netdev *d = net_dev();
    if (!d) {
        kprintf("nincs tamogatott halozati kartya\n");
    } else {
        char ip[20];
        ip_format(net_cfg.ip, ip, sizeof ip);
        kprintf("%s %02x:%02x:%02x:%02x:%02x:%02x   ip %s\n", d->name, d->mac[0], d->mac[1], d->mac[2],
                d->mac[3], d->mac[4], d->mac[5], net_cfg.configured ? ip : "(meg nincs, dhcp fut)");
    }
    label("  hid      ");
    char addr[64];
    if (!bridge_addr(addr, sizeof addr))
        kprintf("nincs cim (write /state/ai/bridge IP:PORT)\n");
    else
        kprintf("%s   PSK %s   %s\n", addr, have_psk_file() ? "van" : "nincs",
                bridge_state > 0 ? "elerheto" : bridge_state < 0 ? "nem valaszol" : "meg nem ellenorzott");
}

static void splash(void)
{
    console_clear();
    console_set_color(CON_BYELLOW, CON_BLACK);
    kprintf("  AO-OS " AO_VERSION);
    console_set_color(CON_BBLACK, CON_BLACK);
    kprintf("   sajat kernelu AI-terminal   ");
    console_set_color(CON_DEFAULT_FG, CON_BLACK);
    kprintf("Aspire One 725\n");
    console_set_color(CON_BBLACK, CON_BLACK);
    for (u32 i = 0; i < 78 && i < console_cols(); i++) kprintf("-");
    kprintf("\n");
    console_set_color(CON_DEFAULT_FG, CON_BLACK);
    print_status();
    console_set_color(CON_BBLACK, CON_BLACK);
    kprintf("  help = parancsok   Tab = kiegeszites   PgUp = boot-uzenetek   /state/rc = indito szkript\n\n");
    console_set_color(CON_DEFAULT_FG, CON_BLACK);
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
    if (blk_present()) {
        u64 ps, pn;
        kprintf("lemez: %s, %lu szektor (%lu MiB)\n", blk_model(), blk_sectors(), blk_sectors() / 2048);
        if (disk_find_partition(&ps, &pn)) kprintf("  AO-particio: LBA %lu, %lu MiB\n", ps, pn / 2048);
        else kprintf("  nincs AO-particio (install)\n");
    }
    kprintf("ramdisk (AOFS v1): %s, %u KiB, %u bejegyzes\n",
            aofs_mounted() ? "csatolva" : "nincs", aofs_image_size() / 1024, aofs_count());
}

static void cmd_install(bool only_mkfs)
{
    if (!blk_present()) { kprintf("install: nincs lemez\n"); return; }
    kprintf("%s: a(z) '%s' lemez %s. Folytatas: ird be, hogy IGEN\n", only_mkfs ? "mkfs" : "install",
            blk_model(), only_mkfs ? "AO-particioja formazodik" : "teljes tartalma torlodik");
    char line[LINE_MAX];
    read_line("> ", line);
    if (strcmp(line, "IGEN") != 0) { kprintf("megszakitva\n"); return; }
    int e = only_mkfs ? disk_mkfs("ao") : disk_install("ao");
    if (e) kprintf("%s: hiba: %s\n", only_mkfs ? "mkfs" : "install", errstr(e));
    else if (only_mkfs) { e = disk_mount_root(); if (e) kprintf("mkfs: csatolas: %s\n", errstr(e)); }
}

/* ---------------------------------------------------------------- halozat */
static void cmd_net(void)
{
    struct netdev *d = net_dev();
    if (!d) { kprintf("nincs halozati eszkoz\n"); return; }
    char ip[20], mask[20], gw[20];
    ip_format(net_cfg.ip, ip, sizeof ip);
    ip_format(net_cfg.mask, mask, sizeof mask);
    ip_format(net_cfg.gw, gw, sizeof gw);
    kprintf("%s  mac %02x:%02x:%02x:%02x:%02x:%02x  link %s\n", d->name,
            d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5], d->up ? "up" : "down");
    if (net_cfg.configured) kprintf("ip %s  mask %s  gw %s\n", ip, mask, gw);
    else kprintf("ip: nincs beallitva (dhcp vagy ip parancs)\n");
    u64 rx, tx, drop;
    net_stats(&rx, &tx, &drop);
    kprintf("rx %lu  tx %lu  eldobva %lu\n", rx, tx, drop);
}

static void cmd_dhcp(void)
{
    kprintf("dhcp...\n");
    console_flush();
    int e = dhcp_run(8000);
    if (e) { kprintf("dhcp: %s\n", errstr(e)); return; }
    cmd_net();
}

static void cmd_ip(int argc, char **argv)
{
    u32 ip, mask, gw = 0;
    if (argc < 3 || !ip_parse(argv[1], &ip) || !ip_parse(argv[2], &mask) || (argc > 3 && !ip_parse(argv[3], &gw))) {
        kprintf("ip CIM MASZK [ATJARO]\n");
        return;
    }
    net_cfg.ip = ip; net_cfg.mask = mask; net_cfg.gw = gw; net_cfg.configured = true;
    cmd_net();
}

static void cmd_ping(const char *arg)
{
    u32 ip;
    if (!arg || !ip_parse(arg, &ip)) { kprintf("ping CIM\n"); return; }
    for (int i = 0; i < 3; i++) {
        u32 rtt;
        int e = net_ping(ip, 2000, &rtt);
        if (e) kprintf("  %s: %s\n", arg, errstr(e));
        else kprintf("  %s: valasz, rtt %u us\n", arg, rtt);
        console_flush();
    }
}

static void cmd_nc(int argc, char **argv)
{
    u32 ip;
    if (argc < 3 || !ip_parse(argv[1], &ip)) { kprintf("nc CIM PORT [szoveg]\n"); return; }
    u32 port = 0;
    for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) port = port * 10 + (u32)(*p - '0');
    int s = tcp_connect(ip, (u16)port, 5000);
    if (s < 0) { kprintf("nc: kapcsolodas: %s\n", errstr(s)); return; }
    kprintf("nc: kapcsolodva (%s)\n", tcp_state_name(s));
    char line[LINE_MAX];
    usize n = 0;
    for (int i = 3; i < argc; i++) n += snformat(line + n, sizeof line - n, "%s%s", argv[i], i + 1 < argc ? " " : "\n");
    if (n) tcp_send(s, line, n);
    for (int i = 0; i < 20; i++) {
        char buf[256];
        isize r = tcp_recv(s, buf, sizeof buf, 2000);
        if (r <= 0) break;
        for (isize k = 0; k < r; k++) console_putc(buf[k]);
        console_flush();
    }
    tcp_close(s);
    kprintf("\nnc: zarva\n");
}

static void cmd_update(void)
{
    if (!blk_present()) { kprintf("update: nincs lemez\n"); return; }
    kprintf("update: a bootloader, a kernel es a programok frissulnek a belso lemezen, a /state megmarad. Folytatas: IGEN\n");
    char line[LINE_MAX];
    read_line("> ", line);
    if (strcmp(line, "IGEN") != 0) { kprintf("megszakitva\n"); return; }
    int e = disk_update();
    if (e) kprintf("update: hiba: %s\n", errstr(e));
}

static void cmd_lastpanic(bool clear)
{
    char buf[512];
    int n = panic_store_read(buf, sizeof buf);
    if (!n) { kprintf("nincs mentett panic\n"); return; }
    kprintf("utolso panic: %s\n", buf);
    if (clear) { panic_store_clear(); kprintf("torolve\n"); }
}

static void cmd_poweroff(void)
{
    kprintf("kikapcsolas...\n");
    console_flush();
    vfs_sync();
    if (!acpi_available()) { kprintf("poweroff: nincs ACPI _S5, hasznald a reboot-ot\n"); return; }
    acpi_poweroff();
    kprintf("poweroff: az ACPI nem kapcsolt ki\n");
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

static void cmd_append(int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    if (!canon(argv[1], path)) { kprintf("append: rossz utvonal\n"); return; }
    char text[LINE_MAX];
    usize n = 0;
    for (int i = 2; i < argc; i++) {
        n += snformat(text + n, sizeof text - n, "%s%s", argv[i], i + 1 < argc ? " " : "\n");
        if (n >= sizeof text) break;
    }
    void *old = NULL;
    usize osz = 0;
    if (vfs_read_all(path, &old, &osz)) { old = NULL; osz = 0; }
    char *buf = kmalloc(osz + n + 2);
    usize total = 0;
    if (osz) { memcpy(buf, old, osz); total = osz; if (buf[total - 1] != '\n') buf[total++] = '\n'; }
    memcpy(buf + total, text, n);
    total += n;
    int e = vfs_write_all(path, buf, total);
    kprintf(e ? "append: %s: %s\n" : "%s: %lu bajt\n", path, e ? errstr(e) : (const char *)(uptr)total);
    kfree(buf);
    if (old) kfree(old);
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
    snformat(alt, sizeof alt, "/rd/bin/%s.aox", name);
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
    if (!quiet_run || status != 0)
        kprintf("[%s: pid %d, rc=%d%s, %lu ms]\n", argv[0], pid, status,
                status == E_TIMEOUT ? " (hatarido/kill)" : status == E_FAULT ? " (kivetel)" : "", tsc_to_ms(dt));
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

/* ai SZOVEG  -> chat-agent;  agent NEV SZOVEG -> /state/agents/NEV.cap vagy /etc/agents/NEV.cap */
static void cmd_agent(const char *name, const char *prog, int argc, char **argv, int first)
{
    if (first > argc) return;
    char mpath[VFS_PATH_MAX];
    struct stat st;
    snformat(mpath, sizeof mpath, "/state/agents/%s.cap", name);
    if (vfs_stat(mpath, &st) != 0) snformat(mpath, sizeof mpath, "/etc/agents/%s.cap", name);
    if (vfs_stat(mpath, &st) != 0) { kprintf("agent: nincs manifest: %s\n", mpath); return; }
    void *text;
    usize len;
    int e = vfs_read_all(mpath, &text, &len);
    if (e) { kprintf("agent: %s: %s\n", mpath, errstr(e)); return; }
    struct capset cs;
    char err[64];
    bool ok = capset_parse(&cs, text, len, err, sizeof err);
    kfree(text);
    if (!ok) { kprintf("agent: manifest hiba: %s\n", err); return; }
    for (int i = 0; net_busy && i < 750; i++) task_sleep_ms(20);   /* boot-kori DHCP/PING: max 15 s */
    char *args[ARGV_MAX + 1];
    int n = 0;
    args[n++] = (char *)prog;
    for (int i = first; i < argc && n < ARGV_MAX; i++) args[n++] = argv[i];
    run_with_caps(&cs, n, args);
}

/* ---------------------------------------------------------------- shot, copy, paste (a hidon at) */
/* a clip-agent (/etc/agents/clip.cap) az agentd-t --file / --clip modban futtatja */
static void run_clip_agent(int argc, char **argv)
{
    quiet_run = true;
    cmd_agent("clip", "agentd", argc, argv, 1);
    quiet_run = false;
}

static void cmd_shot(const char *name)
{
    u32 rows = console_rows(), cols = console_cols();
    usize cap = (usize)rows * (cols * 3 + 2) + 1;
    char *buf = kmalloc(cap);
    usize n = 0;
    for (u32 r = 0; r < rows; r++) {
        console_get_screen_line(r, buf + n, cap - n - 2);
        n += strlen(buf + n);
        buf[n++] = '\n';
    }
    int e = vfs_write_all("/tmp/shot.txt", buf, n);
    kfree(buf);
    if (e) { kprintf("shot: /tmp/shot.txt: %s\n", errstr(e)); return; }
    char *args[] = { "shot", "--file", "shot", (char *)(name ? name : "shot"), "/tmp/shot.txt" };
    run_clip_agent(5, args);
}

static void cmd_copy(const char *nstr)
{
    u64 a = prev_out_start, b = prev_out_end;
    if (nstr) {
        u64 n = 0;
        for (const char *p = nstr; *p >= '0' && *p <= '9'; p++) n = n * 10 + (u64)(*p - '0');
        b = cur_prompt_seq;
        a = b > n ? b - n : 0;
    }
    if (b <= a) { kprintf("copy: nincs mit masolni\n"); return; }
    usize cap = (usize)(b - a) * (console_cols() * 3 + 2) + 1;
    char *buf = kmalloc(cap);
    usize n = 0;
    u32 lines = 0;
    for (u64 s = a; s < b; s++) {
        if (!console_get_line(s, buf + n, cap - n - 2)) continue;
        n += strlen(buf + n);
        buf[n++] = '\n';
        lines++;
    }
    int e = vfs_write_all("/tmp/clip.txt", buf, n);
    kfree(buf);
    if (e) { kprintf("copy: /tmp/clip.txt: %s\n", errstr(e)); return; }
    kprintf("copy: %u sor -> PC\n", lines);
    char *args[] = { "copy", "--file", "clip", "clip", "/tmp/clip.txt" };
    run_clip_agent(5, args);
}

/* fetch NEV [CEL]: a PC share/NEV fajlja; cel alapbol /state/inbox/NEV */
static void cmd_fetch(int argc, char **argv)
{
    if (argc < 2) { kprintf("fetch NEV [CELFAJL]   (a PC-n a share/ mappabol; alap: /state/inbox/NEV)\n"); return; }
    char path[VFS_PATH_MAX];
    if (argc > 2) {
        if (!canon(argv[2], path)) { kprintf("fetch: rossz utvonal\n"); return; }
        struct stat st;
        if (vfs_stat(path, &st) == 0 && st.type == 2) {           /* konyvtar: NEV alaja */
            usize l = strlen(path);
            if (l + strlen(argv[1]) + 2 >= sizeof path) { kprintf("fetch: tul hosszu utvonal\n"); return; }
            if (l > 1) path[l++] = '/';
            strcpy(path + l, argv[1]);
        }
    } else {
        vfs_mkdir("/state/inbox");
        snformat(path, sizeof path, "/state/inbox/%s", argv[1]);
    }
    char *args[] = { "fetch", "--fetch", argv[1], path };
    quiet_run = true;
    cmd_agent("fetch", "agentd", 4, args, 1);
    quiet_run = false;
}

static void cmd_paste(const char *file)
{
    vfs_unlink("/tmp/clip.txt");
    char *args[] = { "paste", "--clip", "/tmp/clip.txt" };
    run_clip_agent(3, args);
    void *b;
    usize n;
    if (vfs_read_all("/tmp/clip.txt", &b, &n)) { kprintf("paste: nem jott vagolap-tartalom\n"); return; }
    if (file) {
        char path[VFS_PATH_MAX];
        if (!canon(file, path)) { kprintf("paste: rossz utvonal\n"); kfree(b); return; }
        int e = vfs_write_all(path, b, n);
        kprintf(e ? "paste: %s: %s\n" : "%s: %lu bajt\n", path, e ? errstr(e) : (const char *)(uptr)n);
    } else {
        const char *p = b;
        usize l = 0;
        while (l < n && l < LINE_MAX - 1 && p[l] != '\n' && p[l] != '\r') { paste_buf[l] = p[l]; l++; }
        paste_buf[l] = 0;
        have_paste = l > 0;
        kprintf("paste: %lu bajt%s\n", n, l < n ? ", az elso sor kerul a parancssorba (fajlba: paste FAJL)" : "");
    }
    kfree(b);
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
    if (!strcmp(c, "help") || !strcmp(c, "/?") || !strcmp(c, "?")) cmd_help(argc > 1 ? argv[1] : NULL);
    else if (!strcmp(c, "status")) print_status();
    else if (!strcmp(c, "time")) cmd_time(argc, argv);
    else if (!strcmp(c, "date")) cmd_date(argc, argv);
    else if (!strcmp(c, "append")) { if (argc > 2) cmd_append(argc, argv); else kprintf("append FAJL SZOVEG...\n"); }
    else if (!strcmp(c, "shot")) cmd_shot(argc > 1 ? argv[1] : NULL);
    else if (!strcmp(c, "copy")) cmd_copy(argc > 1 ? argv[1] : NULL);
    else if (!strcmp(c, "paste")) cmd_paste(argc > 1 ? argv[1] : NULL);
    else if (!strcmp(c, "fetch")) cmd_fetch(argc, argv);
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
    else if (!strcmp(c, "ai")) { if (argc > 1) cmd_agent("chat", "agentd", argc, argv, 1); else kprintf("hasznalat: ai KERDES\n"); }
    else if (!strcmp(c, "agent")) { if (argc > 2) cmd_agent(argv[1], "agentd", argc, argv, 2); else kprintf("agent NEV FELADAT\n"); }
    else if (!strcmp(c, "2048")) { quiet_run = true; cmd_agent("game", "game2048", argc, argv, 1); quiet_run = false; }
    else if (!strcmp(c, "doom")) {
        struct stat st;
        if (vfs_stat("/state/games/doom.aox", &st) != 0)
            kprintf("doom: nincs telepitve. PC-n a share/ mappaban van a doom.aox es a doom1.wad, netbookon:\n"
                    "  fetch doom.aox /state/games\n  fetch doom1.wad /state/games\n");
        else { quiet_run = true; cmd_agent("doom", "/state/games/doom.aox", argc, argv, 1); quiet_run = false; }
    }
    else if (!strcmp(c, "projector")) {
        if (argc > 1) { quiet_run = true; cmd_agent("projector", "projector", argc, argv, 1); quiet_run = false; }
        else kprintf("projector CIM | KERDES  (--dump: szovegkent, teljes kepernyo helyett)\n");
    }
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
    else if (!strcmp(c, "net")) cmd_net();
    else if (!strcmp(c, "dhcp")) cmd_dhcp();
    else if (!strcmp(c, "ip")) cmd_ip(argc, argv);
    else if (!strcmp(c, "ping")) cmd_ping(argc > 1 ? argv[1] : NULL);
    else if (!strcmp(c, "nc")) cmd_nc(argc, argv);
    else if (!strcmp(c, "install")) cmd_install(false);
    else if (!strcmp(c, "mkfs")) cmd_install(true);
    else if (!strcmp(c, "update")) cmd_update();
    else if (!strcmp(c, "sync")) { int e = vfs_sync(); kprintf(e ? "sync: %s\n" : "sync: ok\n", errstr(e)); }
    else if (!strcmp(c, "lastpanic")) cmd_lastpanic(argc > 1 && !strcmp(argv[1], "clear"));
    else if (!strcmp(c, "poweroff")) cmd_poweroff();
    else if (!strcmp(c, "reboot")) cmd_reboot();
    else kprintf("ismeretlen parancs: %s (help)\n", c);
}

/* indito szkript: /state/rc, ha nincs, /etc/rc; soronkent parancs, '#' megjegyzes */
static void run_rc(void)
{
    static const char *const files[] = { "/state/rc", "/etc/rc" };
    for (int f = 0; f < 2; f++) {
        void *b;
        usize n;
        if (vfs_read_all(files[f], &b, &n)) continue;
        const char *p = b;
        usize i = 0;
        while (i < n) {
            usize s = i;
            while (i < n && p[i] != '\n') i++;
            usize l = i - s;
            i++;
            while (l && (p[s + l - 1] == '\r' || p[s + l - 1] == ' ')) l--;
            while (l && p[s] == ' ') { s++; l--; }
            if (!l || p[s] == '#') continue;
            char cmd[LINE_MAX];
            if (l >= LINE_MAX) l = LINE_MAX - 1;
            memcpy(cmd, p + s, l);
            cmd[l] = 0;
            console_set_color(CON_BBLACK, CON_BLACK);
            kprintf("rc> %s\n", cmd);
            console_set_color(CON_DEFAULT_FG, CON_BLACK);
            execute(cmd);
        }
        kfree(b);
        break;
    }
}

void shell_run(const struct bootinfo *bi)
{
    boot = bi;
    char line[LINE_MAX];
    boot_to_prompt_us = tsc_to_us(rdtsc() - bi->boot_tsc);
    build_cmd_names();
    splash();
    run_rc();
    for (;;) {
        char prompt[VFS_PATH_MAX + 8];
        if (strcmp(cwd(), "/") == 0) strcpy(prompt, "AO> ");
        else snformat(prompt, sizeof prompt, "AO %s> ", cwd());
        cur_prompt_seq = console_line_seq();
        read_line_init(prompt, line, have_paste ? paste_buf : NULL);
        have_paste = false;
        if (line[0]) {
            u64 out_start = console_line_seq();
            if (hist_count == 0 || strcmp(history[hist_count - 1], line) != 0) {
                if (hist_count == HIST_MAX) {
                    memmove(history[0], history[1], (HIST_MAX - 1) * LINE_MAX);
                    hist_count--;
                }
                strlcpy(history[hist_count++], line, LINE_MAX);
            }
            bool is_copy = starts_with(line, "copy", 4) && (line[4] == 0 || line[4] == ' ');
            execute(line);
            if (!is_copy) {                                 /* a copy ne irja felul, amit masolna */
                u32 col, row;
                console_get_cursor(&col, &row);
                prev_out_start = out_start;
                prev_out_end = console_line_seq() + (col ? 1 : 0);
            }
        }
        console_flush();
    }
}
