/* agentd: az AI-agent futtatoja ring 3-ban. A manifest capability-keszletevel fut, TCP-n
 * beszel a hiddal (AOP v1, docs/AOP.md), az eszkoz-hivasokat sajat syscallokkal hajtja
 * vegre, igy a kernel minden kerest a cap_check-en ellenoriz.
 *
 *   agentd <feladat szovege...>
 * Hid cime: /state/ai/bridge, majd /etc/ai/bridge ("ip:port"). */
#include "aolib.h"

#define MAGIC "AOP1"
enum { F_HELLO = 1, F_HELLO_OK, F_CONTEXT, F_PROMPT, F_DELTA, F_TOOL_CALL, F_TOOL_RESULT, F_END, F_ERR, F_PING, F_PONG };

static int sock;
static char agent_name[32] = "agent";
static char context_path[96];
static u8 payload[65536];
static char result[16384 + 256];

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
    put_u16(hdr + 6, 0);
    put_u32(hdr + 8, (u32)len);
    int e = send_all(hdr, 12);
    if (e) return e;
    return len ? send_all(data, len) : 0;
}

static int recv_frame(u16 *type, usize *len)
{
    u8 hdr[12];
    int e = recv_all(hdr, 12);
    if (e) return e;
    if (memcmp(hdr, MAGIC, 4) != 0) return E_INVAL;
    *type = get_u16(hdr + 4);
    u32 l = get_u32(hdr + 8);
    if (l >= sizeof payload) return E_LIMIT;
    e = recv_all(payload, l);
    if (e) return e;
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
        if (fd < 0) { ao_printf("  [%s: %s -> %s]\n", name, arg(kvs, n, "path"), ao_errstr(fd)); send_result(id, false, ao_errstr(fd)); return; }
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
        if (fd < 0) { ao_printf("  [fs_write %s -> %s]\n", path, ao_errstr(fd)); send_result(id, false, ao_errstr(fd)); return; }
        const char *content = arg(kvs, n, "content");
        isize w = ao_write(fd, content, strlen(content));
        ao_close(fd);
        ao_printf("  [fs_write %s: %ld bajt]\n", path, (i64)w);
        if (w < 0) send_result(id, false, ao_errstr((int)w));
        else send_result(id, true, "ok");
    } else if (strcmp(name, "fs_list") == 0) {
        struct dirent ents[32];
        int c = ao_list(arg(kvs, n, "path"), ents, 32);
        if (c < 0) { send_result(id, false, ao_errstr(c)); return; }
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
        if (pid < 0) { send_result(id, false, ao_errstr(pid)); return; }
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
        send_result(id, false, "ismeretlen eszkoz");
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

int main(int argc, char **argv)
{
    static char caps[1024], ctx[8192], addr[64], task[1024];
    if (argc < 2) { ao_puts("agentd: feladat szovege kell\n"); return 1; }
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

    ao_printf("[agent %s -> hid %s]\n", agent_name, addr);
    sock = ao_net_connect(addr);
    if (sock < 0) { ao_printf("agentd: kapcsolodas: %s\n", ao_errstr(sock)); return 3; }

    char hello[64];
    usize hl = 0;
    memcpy(hello, "agent=", 6); hl = 6;
    usize nl = strlen(agent_name);
    memcpy(hello + hl, agent_name, nl); hl += nl;
    memcpy(hello + hl, "\nversion=1\n", 11); hl += 11;
    send_frame(F_HELLO, hello, hl);

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
            ao_printf("[hid: %s]\n", (char *)payload);
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
