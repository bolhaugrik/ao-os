/* agentd: az AI-agent futtatoja ring 3-ban. A manifest capability-keszletevel fut, TCP-n
 * beszel a hiddal (AOP v1, docs/AOP.md), az eszkoz-hivasokat sajat syscallokkal hajtja
 * vegre, igy a kernel minden kerest a cap_check-en ellenoriz.
 *
 *   agentd <feladat szovege...>
 * Hid cime: /state/ai/bridge, majd /etc/ai/bridge ("ip:port"). */
#include "aolib.h"
#include "crypto.h"

#define MAGIC "AOP1"
enum { F_HELLO = 1, F_HELLO_OK, F_CONTEXT, F_PROMPT, F_DELTA, F_TOOL_CALL, F_TOOL_RESULT, F_END, F_ERR, F_PING, F_PONG,
       F_FILE, F_CLIP_GET, F_CLIP };
static bool quiet;      /* --file / --clip mod: csak a hid valaszat irjuk ki */
#define FLAG_ENC 1

static int sock;
static char agent_name[32] = "agent";
static char context_path[96];
static u8 payload[65536 + 64];
static u8 sealbuf[65536 + 64];
static char result[16384 + 256];

/* PSK-titkositas (docs/AOP.md): /state/ai/psk = 64 hex karakter */
static u8 psk[32];
static bool have_psk, enc;
static struct aochan chan;
static u8 cnonce[8];

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static bool parse_hex(const char *s, u8 *out, usize n)
{
    for (usize i = 0; i < n; i++) {
        int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (u8)(a * 16 + b);
    }
    return true;
}

static void to_hex(const u8 *in, usize n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (usize i = 0; i < n; i++) { out[2 * i] = d[in[i] >> 4]; out[2 * i + 1] = d[in[i] & 15]; }
    out[2 * n] = 0;
}

/* 8 veletlen bajt: TSC + tick + pid keverve ChaCha20-szal. A nonce egyediseget a hid
 * sajat, jo minosegu veletlenje is biztositja; a biztonsag a PSK-n nyugszik. */
static void rand8(u8 out[8])
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    u8 nonce[12], block[64];
    u64 t = ao_ticks(), p = (u64)ao_getpid();
    nonce[0] = lo & 0xFF; nonce[1] = (lo >> 8) & 0xFF; nonce[2] = (lo >> 16) & 0xFF; nonce[3] = lo >> 24;
    nonce[4] = hi & 0xFF; nonce[5] = (hi >> 8) & 0xFF; nonce[6] = t & 0xFF; nonce[7] = (t >> 8) & 0xFF;
    nonce[8] = (t >> 16) & 0xFF; nonce[9] = (t >> 24) & 0xFF; nonce[10] = p & 0xFF; nonce[11] = (p >> 8) & 0xFF;
    chacha20_block(psk, (u32)(t ^ lo), nonce, block);
    memcpy(out, block + 16, 8);
}

static void put_u16(u8 *p, u16 v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put_u32(u8 *p, u32 v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24; }
static u16 get_u16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 get_u32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static int send_all(const void *buf, usize n)
{
    const u8 *p = buf;
    usize done = 0;
    while (done < n) {
        isize r = ao_write(sock, p + done, n - done);
        if (r <= 0) return (int)(r ? r : E_PIPE);
        done += (usize)r;
    }
    return 0;
}

static int recv_all(void *buf, usize n)
{
    u8 *p = buf;
    usize done = 0;
    while (done < n) {
        isize r = ao_read(sock, p + done, n - done);
        if (r <= 0) return (int)(r ? r : E_PIPE);
        done += (usize)r;
    }
    return 0;
}

static int send_frame(u16 type, const void *data, usize len)
{
    u8 hdr[12];
    memcpy(hdr, MAGIC, 4);
    put_u16(hdr + 4, type);
    put_u16(hdr + 6, enc ? FLAG_ENC : 0);
    put_u32(hdr + 8, (u32)(enc ? len + 16 : len));
    int e = send_all(hdr, 12);
    if (e) return e;
    if (!enc) return len ? send_all(data, len) : 0;
    if (len + 16 > sizeof sealbuf) return E_LIMIT;
    aochan_seal(&chan, hdr, data, len, sealbuf);       /* AAD = a fejlec */
    return send_all(sealbuf, len + 16);
}

static int recv_frame(u16 *type, usize *len)
{
    u8 hdr[12];
    int e = recv_all(hdr, 12);
    if (e) return e;
    if (memcmp(hdr, MAGIC, 4) != 0) return E_INVAL;
    *type = get_u16(hdr + 4);
    u16 flags = get_u16(hdr + 6);
    u32 l = get_u32(hdr + 8);
    if (l >= sizeof payload - 16) return E_LIMIT;
    e = recv_all(payload, l);
    if (e) return e;
    if (flags & FLAG_ENC) {
        if (!enc) return E_INVAL;                           /* titkositott keret kezfogas nelkul */
        if (!aochan_open(&chan, hdr, payload, l, payload)) return E_INVAL;   /* rossz tag/szamlalo */
        l -= 16;
    } else if (enc && *type != F_ERR) {
        return E_INVAL;                                     /* titkositatlan keret a csatornan */
    }
    payload[l] = 0;
    *len = l;
    return 0;
}

/* ---------------------------------------------------------------- eszkozok */
struct kv { const char *key; const char *val; usize vlen; };

/* "id\nname\nkey\nlen\nvalue\n..." feldolgozasa helyben (a payload-ot modositja) */
static int parse_call(usize len, char **id, char **name, struct kv *kvs, int maxkv)
{
    char *p = (char *)payload;
    char *end = p + len;
    *id = p;
    while (p < end && *p != '\n') p++;
    if (p >= end) return -1;
    *p++ = 0;
    *name = p;
    while (p < end && *p != '\n') p++;
    if (p >= end) return -1;
    *p++ = 0;
    int n = 0;
    while (p < end && n < maxkv) {
        kvs[n].key = p;
        while (p < end && *p != '\n') p++;
        if (p >= end) break;
        *p++ = 0;
        usize vl = 0;
        while (p < end && *p >= '0' && *p <= '9') vl = vl * 10 + (usize)(*p++ - '0');
        if (p >= end || *p != '\n') break;
        p++;
        if ((usize)(end - p) < vl) break;
        kvs[n].val = p;
        kvs[n].vlen = vl;
        p += vl;
        if (p < end && *p == '\n') { *p = 0; p++; }
        n++;
    }
    return n;
}

static const char *arg(struct kv *kvs, int n, const char *key)
{
    for (int i = 0; i < n; i++)
        if (strcmp(kvs[i].key, key) == 0) return kvs[i].val;
    return "";
}

/* hibakod -> a modellnek szolo magyarazat */
static const char *explain(int e)
{
    switch (e) {
    case E_CAP: return "E_CAP: nincs jogosultsag ehhez az utvonalhoz/muvelethez (lasd a capability-listat); ne probald ujra";
    case E_NOENT: return "E_NOENT: nincs ilyen fajl vagy konyvtar (a szulo konyvtar sem letezik? fs_mkdir-rel hozhato letre)";
    case E_ISDIR: return "E_ISDIR: ez konyvtar, nem fajl";
    case E_NOTDIR: return "E_NOTDIR: az utvonal egy eleme nem konyvtar";
    case E_EXIST: return "E_EXIST: mar letezik";
    case E_ROFS: return "E_ROFS: csak olvashato fajlrendszer (/rd, /sys)";
    case E_LIMIT: return "E_LIMIT: eroforras-korlat";
    case E_INVAL: return "E_INVAL: ervenytelen parameter (abszolut utvonal kell, max 127 karakter)";
    default: return ao_errstr(e);
    }
}

/* szulo konyvtarak letrehozasa (mkdir -p), minden lepes a cap_check-en megy at */
static int mkdir_parents(const char *path)
{
    char tmp[128];
    usize l = strlen(path);
    if (l >= sizeof tmp) return E_INVAL;
    memcpy(tmp, path, l + 1);
    for (usize i = 1; i < l; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = 0;
        struct stat st;
        if (ao_stat(tmp, &st) != 0) {
            int e = ao_mkdir(tmp);
            if (e && e != E_EXIST) { return e; }
            ao_printf("  [fs_mkdir %s]\n", tmp);
        }
        tmp[i] = '/';
    }
    return 0;
}

static void send_result(const char *id, bool ok, const char *text)
{
    usize il = strlen(id), tl = strlen(text);
    usize total = il + 1 + (ok ? 2 : 5) + 1 + tl;
    if (total > sizeof result) { tl -= total - sizeof result; total = sizeof result; }
    char *p = result;
    memcpy(p, id, il); p += il; *p++ = '\n';
    memcpy(p, ok ? "ok" : "error", ok ? 2 : 5); p += ok ? 2 : 5; *p++ = '\n';
    memcpy(p, text, tl); p += tl;
    send_frame(F_TOOL_RESULT, result, (usize)(p - result));
}

static void run_tool(usize len)
{
    char *id, *name;
    struct kv kvs[6];
    int n = parse_call(len, &id, &name, kvs, 6);
    if (n < 0) return;
    char buf[16384];

    if (strcmp(name, "fs_read") == 0) {
        int fd = ao_open(arg(kvs, n, "path"), O_READ);
        if (fd < 0) { ao_printf("  [%s: %s -> %s]\n", name, arg(kvs, n, "path"), ao_errstr(fd)); send_result(id, false, explain(fd)); return; }
        usize got = 0;
        for (;;) {
            isize r = ao_read(fd, buf + got, sizeof buf - 1 - got);
            if (r <= 0 || got + (usize)r >= sizeof buf - 1) { if (r > 0) got += (usize)r; break; }
            got += (usize)r;
        }
        ao_close(fd);
        buf[got] = 0;
        ao_printf("  [fs_read %s: %lu bajt]\n", arg(kvs, n, "path"), (u64)got);
        send_result(id, true, buf);
    } else if (strcmp(name, "fs_write") == 0) {
        const char *path = arg(kvs, n, "path");
        int fd = ao_open(path, O_WRITE | O_CREATE | O_TRUNC);
        if (fd == E_NOENT) {
            int e = mkdir_parents(path);
            if (e == 0) fd = ao_open(path, O_WRITE | O_CREATE | O_TRUNC);
            else fd = e;
        }
        if (fd < 0) { ao_printf("  [fs_write %s -> %s]\n", path, ao_errstr(fd)); send_result(id, false, explain(fd)); return; }
        const char *content = arg(kvs, n, "content");
        isize w = ao_write(fd, content, strlen(content));
        ao_close(fd);
        ao_printf("  [fs_write %s: %ld bajt]\n", path, (i64)w);
        if (w < 0) send_result(id, false, explain((int)w));
        else send_result(id, true, "ok, fajl irva");
    } else if (strcmp(name, "fs_mkdir") == 0) {
        const char *path = arg(kvs, n, "path");
        int e = ao_mkdir(path);
        if (e == E_NOENT) { e = mkdir_parents(path); if (e == 0) e = ao_mkdir(path); }
        ao_printf("  [fs_mkdir %s -> %s]\n", path, e ? ao_errstr(e) : "ok");
        if (e && e != E_EXIST) send_result(id, false, explain(e));
        else send_result(id, true, e == E_EXIST ? "mar letezett" : "ok, konyvtar letrehozva");
    } else if (strcmp(name, "fs_list") == 0) {
        struct dirent ents[32];
        int c = ao_list(arg(kvs, n, "path"), ents, 32);
        if (c < 0) { ao_printf("  [fs_list %s -> %s]\n", arg(kvs, n, "path"), ao_errstr(c)); send_result(id, false, explain(c)); return; }
        usize pos = 0;
        for (int i = 0; i < c && pos + 80 < sizeof buf; i++) {
            usize l = strlen(ents[i].name);
            memcpy(buf + pos, ents[i].name, l); pos += l;
            if (ents[i].type == 2) buf[pos++] = '/';
            buf[pos++] = '\n';
        }
        buf[pos] = 0;
        ao_printf("  [fs_list %s: %d bejegyzes]\n", arg(kvs, n, "path"), c);
        send_result(id, true, buf);
    } else if (strcmp(name, "task_run") == 0) {
        const char *prog = arg(kvs, n, "program");
        char *argv[8];
        char argbuf[256];
        int argc = 0;
        argv[argc++] = (char *)prog;
        const char *a = arg(kvs, n, "args");
        usize al = strlen(a);
        if (al >= sizeof argbuf) al = sizeof argbuf - 1;
        memcpy(argbuf, a, al);
        argbuf[al] = 0;
        char *p = argbuf;
        while (*p && argc < 7) {
            while (*p == ' ') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = 0;
        }
        argv[argc] = NULL;
        char path[96];
        usize pl = 0;
        if (prog[0] != '/') { memcpy(path, "/bin/", 5); pl = 5; }
        usize l = strlen(prog);
        memcpy(path + pl, prog, l); pl += l;
        if (l < 4 || strcmp(prog + l - 4, ".aox") != 0) { memcpy(path + pl, ".aox", 4); pl += 4; }
        path[pl] = 0;
        ao_printf("  [task_run %s]\n", path);
        int pid = ao_spawn(path, argv, NULL);
        if (pid < 0) {
            send_result(id, false, pid == E_NOENT ? "E_NOENT: nincs ilyen program a /bin alatt (nincs shell, sh, busybox, mkdir program; konyvtarhoz fs_mkdir)" : explain(pid));
            return;
        }
        int status = 0;
        ao_wait(pid, &status);
        char rc[32];
        char *q = rc;
        memcpy(q, "rc=", 3); q += 3;
        int v = status < 0 ? -status : status;
        if (status < 0) *q++ = '-';
        char tmp[12]; int tl = 0;
        do { tmp[tl++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (tl) *q++ = tmp[--tl];
        *q = 0;
        send_result(id, true, rc);
    } else if (strcmp(name, "ask_user") == 0) {
        ao_printf("\n? %s\n> ", arg(kvs, n, "question"));
        ao_gets(buf, 512);
        send_result(id, true, buf);
    } else if (strcmp(name, "done") == 0) {
        ao_printf("\n[kesz: %s]\n", arg(kvs, n, "summary"));
        send_result(id, true, "ok");
    } else {
        send_result(id, false, "ismeretlen eszkoz (elerheto: fs_read, fs_write, fs_mkdir, fs_list, task_run, ask_user, done)");
    }
}

/* ---------------------------------------------------------------- kontextus */
static usize read_file(const char *path, char *buf, usize cap)
{
    int fd = ao_open(path, O_READ);
    if (fd < 0) return 0;
    usize got = 0;
    for (;;) {
        isize r = ao_read(fd, buf + got, cap - 1 - got);
        if (r <= 0) break;
        got += (usize)r;
        if (got >= cap - 1) break;
    }
    ao_close(fd);
    buf[got] = 0;
    return got;
}

/* --file KIND NEV UTVONAL: a fajl a PC-re megy (shot: shots/ mappa, clip: vagolap) */
static int do_file(const char *kind, const char *name, const char *path)
{
    static char fb[48 * 1024];
    usize kl = strlen(kind), nl = strlen(name);
    if (kl + nl + 2 > 128) return 1;
    memcpy(fb, kind, kl); fb[kl] = '\n';
    memcpy(fb + kl + 1, name, nl); fb[kl + 1 + nl] = '\n';
    usize hl = kl + nl + 2;
    int fd = ao_open(path, O_READ);
    if (fd < 0) { ao_printf("agentd: %s: %s\n", path, ao_errstr(fd)); return 3; }
    usize n = hl;
    for (;;) {
        isize r = ao_read(fd, fb + n, sizeof fb - n);
        if (r <= 0 || n + (usize)r >= sizeof fb) { if (r > 0) n += (usize)r; break; }
        n += (usize)r;
    }
    ao_close(fd);
    send_frame(F_FILE, fb, n);
    int rc = 0;
    for (;;) {
        u16 type;
        usize len;
        int e = recv_frame(&type, &len);
        if (e) { ao_printf("agentd: kapcsolat: %s\n", ao_errstr(e)); rc = 4; break; }
        if (type == F_DELTA) { ao_write(1, payload, len); ao_puts("\n"); }
        else if (type == F_END) break;
        else if (type == F_ERR) { ao_printf("[hid hiba: %s]\n", (char *)payload); rc = 5; break; }
        else if (type == F_PING) send_frame(F_PONG, NULL, 0);
    }
    return rc;
}

/* --clip UTVONAL: a PC vagolapja a fajlba */
static int do_clip(const char *path)
{
    send_frame(F_CLIP_GET, NULL, 0);
    for (;;) {
        u16 type;
        usize len;
        int e = recv_frame(&type, &len);
        if (e) { ao_printf("agentd: kapcsolat: %s\n", ao_errstr(e)); return 4; }
        if (type == F_CLIP) {
            int fd = ao_open(path, O_WRITE | O_CREATE | O_TRUNC);
            if (fd < 0) { ao_printf("agentd: %s: %s\n", path, ao_errstr(fd)); return 3; }
            ao_write(fd, payload, len);
            ao_close(fd);
            return 0;
        }
        if (type == F_ERR) { ao_printf("[hid hiba: %s]\n", (char *)payload); return 5; }
        if (type == F_PING) send_frame(F_PONG, NULL, 0);
    }
}

int main(int argc, char **argv)
{
    static char caps[1024], ctx[8192], addr[64], task[1024];
    if (argc < 2) { ao_puts("agentd: feladat szovege kell\n"); return 1; }
    bool mode_file = strcmp(argv[1], "--file") == 0, mode_clip = strcmp(argv[1], "--clip") == 0;
    if (mode_file && argc < 5) { ao_puts("agentd --file KIND NEV UTVONAL\n"); return 1; }
    if (mode_clip && argc < 3) { ao_puts("agentd --clip UTVONAL\n"); return 1; }
    quiet = mode_file || mode_clip;
    usize tl = 0;
    for (int i = 1; i < argc && tl + strlen(argv[i]) + 2 < sizeof task; i++) {
        usize l = strlen(argv[i]);
        memcpy(task + tl, argv[i], l); tl += l;
        task[tl++] = ' ';
    }
    if (tl) tl--;
    task[tl] = 0;

    int cn = ao_getcaps(caps, sizeof caps);
    if (cn > 0) {
        /* elso sor: "agent NEV" */
        usize i = 6, j = 0;
        while (caps[i] && caps[i] != '\n' && j < sizeof agent_name - 1) agent_name[j++] = caps[i++];
        agent_name[j] = 0;
    }
    if (!read_file("/state/ai/bridge", addr, sizeof addr) && !read_file("/etc/ai/bridge", addr, sizeof addr)) {
        ao_puts("agentd: nincs hid-cim (/state/ai/bridge: ip:port)\n");
        return 2;
    }
    for (usize i = 0; addr[i]; i++) if (addr[i] == '\n' || addr[i] == '\r') { addr[i] = 0; break; }

    /* PSK: /state/ai/psk (64 hex). Ha van, a kezfogas utan minden keret titkositott. */
    static char pskhex[80];
    if (read_file("/state/ai/psk", pskhex, sizeof pskhex) >= 64 && parse_hex(pskhex, psk, 32))
        have_psk = true;

    if (!quiet) ao_printf("[agent %s -> hid %s%s]\n", agent_name, addr, have_psk ? ", PSK" : "");
    sock = ao_net_connect(addr);
    if (sock < 0) { ao_printf("agentd: kapcsolodas: %s\n", ao_errstr(sock)); return 3; }

    char hello[96];
    usize hl = 0;
    memcpy(hello, "agent=", 6); hl = 6;
    usize nl = strlen(agent_name);
    memcpy(hello + hl, agent_name, nl); hl += nl;
    memcpy(hello + hl, "\nversion=1\n", 11); hl += 11;
    if (have_psk) {
        rand8(cnonce);
        memcpy(hello + hl, "nonce=", 6); hl += 6;
        to_hex(cnonce, 8, hello + hl); hl += 16;
        hello[hl++] = '\n';
    }
    send_frame(F_HELLO, hello, hl);

    /* kezfogas: HELLO_OK (vagy ERR) elott nem kuldunk tobbet */
    {
        u16 type;
        usize len;
        int e = recv_frame(&type, &len);
        if (e) { ao_printf("agentd: kezfogas: %s\n", ao_errstr(e)); ao_close(sock); return 4; }
        if (type == F_ERR) { ao_printf("[hid hiba: %s]\n", (char *)payload); ao_close(sock); return 5; }
        if (type != F_HELLO_OK) { ao_puts("agentd: varatlan valasz a kezfogasban\n"); ao_close(sock); return 4; }
        const char *p = (const char *)payload;
        const char *np = NULL;
        bool server_enc = false;
        for (usize i = 0; i + 6 <= len; i++) {
            if ((i == 0 || p[i - 1] == '\n') && memcmp(p + i, "nonce=", 6) == 0) np = p + i + 6;
            if ((i == 0 || p[i - 1] == '\n') && memcmp(p + i, "enc=1", 5) == 0) server_enc = true;
        }
        for (usize i = 0; i < len; i++) if (payload[i] == '\n') { payload[i] = 0; break; }
        if (have_psk) {
            u8 snonce[8];
            if (!server_enc || !np || !parse_hex(np, snonce, 8)) {
                ao_puts("agentd: a netbookon van PSK, de a hid nem titkosit (inditsd --psk-file kapcsoloval)\n");
                ao_close(sock);
                return 6;
            }
            aochan_init(&chan, psk, cnonce, snonce, false);
            enc = true;
            if (!quiet) ao_printf("[hid: %s, titkositott csatorna]\n", (char *)payload);
        } else if (!quiet) {
            ao_printf("[hid: %s]\n", (char *)payload);
        }
    }
    if (mode_file || mode_clip) {
        int rc = mode_file ? do_file(argv[2], argv[3], argv[4]) : do_clip(argv[2]);
        ao_close(sock);
        return rc;
    }

    /* kontextus: capability-lista + mentett kontextus */
    usize cl = 0;
    memcpy(ctx, "Capability-lista (a kernel ezt kenyszeriti ki):\n", 48); cl = 48;
    if (cn > 0) { memcpy(ctx + cl, caps, (usize)cn); cl += (usize)cn; }
    usize pl = 0;
    const char *pre = "/state/agents/";
    memcpy(context_path, pre, 14); pl = 14;
    memcpy(context_path + pl, agent_name, nl); pl += nl;
    memcpy(context_path + pl, "/context.txt", 13);
    usize saved = read_file(context_path, ctx + cl + 40, sizeof ctx - cl - 41);
    if (saved) {
        memcpy(ctx + cl, "\nKorabbi feladatok es eredmenyek:\n", 34);
        /* a fajl tartalma cl+40-tol kezdodik; 6 bajt res marad, nem baj */
        cl = cl + 40 + saved;
    }
    send_frame(F_CONTEXT, ctx, cl);
    send_frame(F_PROMPT, task, tl);

    int rc = 0;
    static char last[4096];
    usize ll = 0;
    for (;;) {
        u16 type;
        usize len;
        int e = recv_frame(&type, &len);
        if (e) { ao_printf("\nagentd: kapcsolat: %s\n", ao_errstr(e)); rc = 4; break; }
        if (type == F_HELLO_OK) {
            continue;                                   /* mar feldolgoztuk a kezfogasban */
        } else if (type == F_DELTA) {
            ao_write(1, payload, len);
            usize c = len < sizeof last - 1 - ll ? len : sizeof last - 1 - ll;
            memcpy(last + ll, payload, c); ll += c;
        } else if (type == F_TOOL_CALL) {
            run_tool(len);
        } else if (type == F_END) {
            ao_puts("\n");
            break;
        } else if (type == F_ERR) {
            ao_printf("\n[hid hiba: %s]\n", (char *)payload);
            rc = 5;
            break;
        } else if (type == F_PING) {
            send_frame(F_PONG, NULL, 0);
        }
    }
    ao_close(sock);

    /* kontextus mentese, ha van ra jog (best effort) */
    if (rc == 0) {
        int fd = ao_open(context_path, O_WRITE | O_CREATE | O_APPEND);
        if (fd >= 0) {
            ao_write(fd, "## feladat\n", 11);
            ao_write(fd, task, tl);
            ao_write(fd, "\n## eredmeny\n", 13);
            last[ll] = 0;
            ao_write(fd, last, ll);
            ao_write(fd, "\n", 1);
            ao_close(fd);
        }
    }
    return rc;
}
