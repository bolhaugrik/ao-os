/* agentd: az AI-agent futtatoja ring 3-ban. A manifest capability-keszletevel fut, TCP-n
 * beszel a hiddal (AOP, docs/AOP.md, kliens: aop.c), az eszkoz-hivasokat sajat syscallokkal
 * hajtja vegre, igy a kernel minden kerest a cap_check-en ellenoriz.
 *
 *   agentd <feladat szovege...>
 *   agentd --file KIND NEV UTVONAL     a fajl a PC-re (shot: shots/ mappa, clip: vagolap)
 *   agentd --clip UTVONAL              a PC vagolapja a fajlba
 * Hid cime: /state/ai/bridge, majd /etc/ai/bridge ("ip:port"). */
#include "aop.h"

static bool quiet;      /* --file / --clip mod: csak a hid valaszat irjuk ki */
static char agent_name[32] = "agent";
static char context_path[96];
static char result[16384 + 256];

/* ---------------------------------------------------------------- eszkozok */
struct kv { const char *key; const char *val; usize vlen; };

/* "id\nname\nkey\nlen\nvalue\n..." feldolgozasa helyben (a payload-ot modositja) */
static int parse_call(usize len, char **id, char **name, struct kv *kvs, int maxkv)
{
    char *p = (char *)aop_payload;
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
    aop_send(AOP_TOOL_RESULT, result, (usize)(p - result));
}

static void run_tool(usize len)
{
    char *id, *name;
    struct kv kvs[6];
    int n = parse_call(len, &id, &name, kvs, 6);
    if (n < 0) return;
    static char buf[16384];

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
    /* darabokban: FILE(kind, nev, darab)..., majd FILE("done", nev, ures); a hid osszerakja */
    static char fb[48 * 1024];
    usize kl = strlen(kind), nl = strlen(name);
    if (kl + nl + 2 > 128) return 1;
    memcpy(fb, kind, kl); fb[kl] = '\n';
    memcpy(fb + kl + 1, name, nl); fb[kl + 1 + nl] = '\n';
    usize hl = kl + nl + 2;
    int fd = ao_open(path, O_READ);
    if (fd < 0) { ao_printf("agentd: %s: %s\n", path, ao_errstr(fd)); return 3; }
    u64 t0 = ao_ticks(), total = 0;
    for (;;) {
        usize n = hl;
        while (n < sizeof fb) {
            isize r = ao_read(fd, fb + n, sizeof fb - n);
            if (r <= 0) break;
            n += (usize)r;
        }
        if (n == hl) break;
        int e = aop_send(AOP_FILE, fb, n);
        if (e) { ao_printf("agentd: kuldes: %s\n", ao_errstr(e)); ao_close(fd); return 4; }
        total += n - hl;
        if (n < sizeof fb) break;
    }
    ao_close(fd);
    if (total > 65536) ao_printf("%s: %lu KiB elkuldve, %lu ms\n", path, total / 1024, (ao_ticks() - t0) * 10);
    memcpy(fb, "done\n", 5);
    memcpy(fb + 5, name, nl); fb[5 + nl] = '\n';
    aop_send(AOP_FILE, fb, 6 + nl);
    int rc = 0;
    for (;;) {
        u16 type;
        usize len;
        int e = aop_recv(&type, &len);
        if (e) { ao_printf("agentd: kapcsolat: %s\n", ao_errstr(e)); rc = 4; break; }
        if (type == AOP_DELTA) { ao_write(1, aop_payload, len); ao_puts("\n"); }
        else if (type == AOP_END) break;
        else if (type == AOP_ERR) { ao_printf("[hid hiba: %s]\n", (char *)aop_payload); rc = 5; break; }
    }
    return rc;
}

/* --fetch NEV UTVONAL: a PC share/NEV fajlja darabokban a netbook fajljaba */
static int do_fetch(const char *name, const char *path)
{
    char req[160];
    usize nl = strlen(name);
    if (nl > 120) { ao_puts("agentd: tul hosszu nev\n"); return 1; }
    memcpy(req, "name=", 5);
    memcpy(req + 5, name, nl);
    req[5 + nl] = '\n';
    aop_send(AOP_FETCH, req, 6 + nl);
    int fd = -1;
    u64 total = 0;
    u64 t0 = ao_ticks();
    for (;;) {
        u16 type;
        usize len;
        int e = aop_recv(&type, &len);
        if (e) { ao_printf("agentd: kapcsolat: %s\n", ao_errstr(e)); if (fd >= 0) ao_close(fd); return 4; }
        if (type == AOP_FILE) {
            /* "data\nNEV\n" + tartalom */
            usize p = 0, nls = 0;
            while (p < len && nls < 2) { if (aop_payload[p] == '\n') nls++; p++; }
            if (nls < 2) continue;
            if (fd < 0) {
                fd = ao_open(path, O_WRITE | O_CREATE | O_TRUNC);
                if (fd < 0) { ao_printf("agentd: %s: %s\n", path, ao_errstr(fd)); return 3; }
            }
            usize n = len - p, done = 0;
            while (done < n) {
                isize w = ao_write(fd, aop_payload + p + done, n - done);
                if (w <= 0) { ao_printf("agentd: iras: %s\n", ao_errstr((int)w)); ao_close(fd); return 3; }
                done += (usize)w;
            }
            total += n;
            if ((total / 65536) % 4 == 0) { ao_printf("\r  %lu KiB", total / 1024); }
        } else if (type == AOP_END) {
            break;
        } else if (type == AOP_ERR) {
            ao_printf("[hid hiba: %s]\n", (char *)aop_payload);
            if (fd >= 0) ao_close(fd);
            return 5;
        }
    }
    if (fd < 0) {
        fd = ao_open(path, O_WRITE | O_CREATE | O_TRUNC);      /* ures fajl */
        if (fd < 0) { ao_printf("agentd: %s: %s\n", path, ao_errstr(fd)); return 3; }
    }
    ao_close(fd);
    u64 ms = (ao_ticks() - t0) * 10;
    ao_printf("\r%s: %lu bajt, %lu ms%s\n", path, total, ms, ms ? "" : "");
    return 0;
}

/* --clip UTVONAL: a PC vagolapja a fajlba */
static int do_clip(const char *path)
{
    aop_send(AOP_CLIP_GET, NULL, 0);
    for (;;) {
        u16 type;
        usize len;
        int e = aop_recv(&type, &len);
        if (e) { ao_printf("agentd: kapcsolat: %s\n", ao_errstr(e)); return 4; }
        if (type == AOP_CLIP) {
            int fd = ao_open(path, O_WRITE | O_CREATE | O_TRUNC);
            if (fd < 0) { ao_printf("agentd: %s: %s\n", path, ao_errstr(fd)); return 3; }
            ao_write(fd, aop_payload, len);
            ao_close(fd);
            return 0;
        }
        if (type == AOP_ERR) { ao_printf("[hid hiba: %s]\n", (char *)aop_payload); return 5; }
    }
}

int main(int argc, char **argv)
{
    static char caps[1024], ctx[8192], task[1024];
    if (argc < 2) { ao_puts("agentd: feladat szovege kell\n"); return 1; }
    bool mode_file = strcmp(argv[1], "--file") == 0, mode_clip = strcmp(argv[1], "--clip") == 0;
    bool mode_fetch = strcmp(argv[1], "--fetch") == 0;
    if (mode_file && argc < 5) { ao_puts("agentd --file KIND NEV UTVONAL\n"); return 1; }
    if (mode_clip && argc < 3) { ao_puts("agentd --clip UTVONAL\n"); return 1; }
    if (mode_fetch && argc < 4) { ao_puts("agentd --fetch NEV UTVONAL\n"); return 1; }
    quiet = mode_file || mode_clip || mode_fetch;
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
    int e = aop_connect(agent_name, quiet);
    if (e) return e;
    if (mode_file || mode_clip || mode_fetch) {
        int rc = mode_file ? do_file(argv[2], argv[3], argv[4]) : mode_fetch ? do_fetch(argv[2], argv[3]) : do_clip(argv[2]);
        aop_close();
        return rc;
    }

    /* kontextus: capability-lista + mentett kontextus */
    usize cl = 0;
    memcpy(ctx, "Capability-lista (a kernel ezt kenyszeriti ki):\n", 48); cl = 48;
    if (cn > 0) { memcpy(ctx + cl, caps, (usize)cn); cl += (usize)cn; }
    usize nl = strlen(agent_name);
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
    aop_send(AOP_CONTEXT, ctx, cl);
    aop_send(AOP_PROMPT, task, tl);

    int rc = 0;
    static char last[4096];
    usize ll = 0;
    for (;;) {
        u16 type;
        usize len;
        e = aop_recv(&type, &len);
        if (e) { ao_printf("\nagentd: kapcsolat: %s\n", ao_errstr(e)); rc = 4; break; }
        if (type == AOP_HELLO_OK) {
            continue;                                   /* mar feldolgoztuk a kezfogasban */
        } else if (type == AOP_DELTA) {
            ao_write(1, aop_payload, len);
            usize c = len < sizeof last - 1 - ll ? len : sizeof last - 1 - ll;
            memcpy(last + ll, aop_payload, c); ll += c;
        } else if (type == AOP_TOOL_CALL) {
            run_tool(len);
        } else if (type == AOP_END) {
            ao_puts("\n");
            break;
        } else if (type == AOP_ERR) {
            ao_printf("\n[hid hiba: %s]\n", (char *)aop_payload);
            rc = 5;
            break;
        }
    }
    aop_close();

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
