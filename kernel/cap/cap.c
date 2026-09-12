#include "cap.h"
#include "syscall.h"
#include "../task/task.h"
#include "../cpu/pit.h"
#include "../lib/string.h"
#include "../lib/fmt.h"

static const char *kind_names[CAP_MAX_KIND] = {
    "none", "fs.read", "fs.write", "exec", "net", "spawn", "sysinfo", "power", "console",
};

const char *cap_kind_name(enum cap_kind k) { return k < CAP_MAX_KIND ? kind_names[k] : "?"; }

int cap_kind_from_name(const char *s)
{
    for (int i = 1; i < CAP_MAX_KIND; i++)
        if (strcmp(kind_names[i], s) == 0)
            return i;
    return -1;
}

static void add(struct capset *cs, u8 kind, const char *pattern)
{
    if (cs->n >= CAP_MAX) return;
    cs->caps[cs->n].kind = kind;
    strlcpy(cs->caps[cs->n].pattern, pattern ? pattern : "", CAP_PATTERN_MAX);
    cs->n++;
}

void capset_root(struct capset *cs)
{
    memset(cs, 0, sizeof *cs);
    strcpy(cs->name, "root");
    add(cs, CAP_FS_READ, "**");
    add(cs, CAP_FS_WRITE, "**");
    add(cs, CAP_EXEC, "**");
    add(cs, CAP_NET, "*");
    add(cs, CAP_TASK_SPAWN, "");
    add(cs, CAP_SYS_INFO, "");
    add(cs, CAP_SYS_POWER, "");
    add(cs, CAP_CONSOLE, "");
}

/* ---------------------------------------------------------------- glob */
bool cap_glob(const char *p, const char *s)
{
    if (p[0] == '/' && p[1] == '*' && p[2] == '*' && p[3] == 0 && *s == 0)
        return true;                           /* a "/x/ + **" minta illik "/x"-re is */
    while (*p) {
        if (p[0] == '*' && p[1] == '*') {
            p += 2;
            if (*p == '/') p++;
            if (!*p) return true;
            for (const char *t = s;; t++) {
                if (cap_glob(p, t)) return true;
                if (!*t) return false;
            }
        }
        if (*p == '*') {
            p++;
            for (const char *t = s;; t++) {
                if (cap_glob(p, t)) return true;
                if (!*t || *t == '/') return false;
            }
        }
        if (*p != *s) return false;
        p++;
        s++;
    }
    return *s == 0;
}

/* ---------------------------------------------------------------- manifest */
static u64 parse_size(const char *v)
{
    u64 n = 0;
    while (*v >= '0' && *v <= '9') n = n * 10 + (u64)(*v++ - '0');
    if (*v == 'K' || *v == 'k') n *= 1024;
    else if (*v == 'M' || *v == 'm') n *= 1024 * 1024;
    else if (*v == 'G' || *v == 'g') n *= 1024ULL * 1024 * 1024;
    return n;
}

static u64 parse_ms(const char *v)
{
    u64 n = 0;
    while (*v >= '0' && *v <= '9') n = n * 10 + (u64)(*v++ - '0');
    if (*v == 's') n *= 1000;
    else if (*v == 'm' && v[1] == 0) n *= 60000;
    return n;
}

bool capset_parse(struct capset *cs, const char *text, usize len, char *err, usize errlen)
{
    memset(cs, 0, sizeof *cs);
    strcpy(cs->name, "agent");
    usize i = 0;
    int lineno = 0;
    while (i < len) {
        char line[128];
        usize n = 0;
        while (i < len && text[i] != '\n') {
            if (n < sizeof line - 1) line[n++] = text[i];
            i++;
        }
        i++;
        line[n] = 0;
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        char *key = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
        while (*p == ' ' || *p == '\t') p++;
        char *val = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '#') p++;
        *p = 0;

        if (!strcmp(key, "agent")) { strlcpy(cs->name, val, sizeof cs->name); continue; }
        if (!strcmp(key, "mem")) { cs->limits.mem_bytes = parse_size(val); continue; }
        if (!strcmp(key, "cpu")) { cs->limits.cpu_ms = parse_ms(val); continue; }
        if (!strcmp(key, "deadline")) { cs->limits.deadline_ms = parse_ms(val); continue; }
        int kind = cap_kind_from_name(key);
        if (kind < 0) {
            snformat(err, errlen, "%d. sor: ismeretlen kulcs '%s'", lineno, key);
            return false;
        }
        if ((kind == CAP_FS_READ || kind == CAP_FS_WRITE || kind == CAP_EXEC || kind == CAP_NET) && !*val) {
            snformat(err, errlen, "%d. sor: '%s' mintat var", lineno, key);
            return false;
        }
        if (cs->n >= CAP_MAX) {
            snformat(err, errlen, "%d. sor: tul sok capability (max %d)", lineno, CAP_MAX);
            return false;
        }
        add(cs, (u8)kind, val);
    }
    return true;
}

bool capset_subset(const struct capset *c, const struct capset *p)
{
    for (u32 i = 0; i < c->n; i++) {
        bool ok = false;
        for (u32 j = 0; j < p->n && !ok; j++)
            if (p->caps[j].kind == c->caps[i].kind &&
                (p->caps[j].pattern[0] == 0 || cap_glob(p->caps[j].pattern, c->caps[i].pattern)))
                ok = true;
        if (!ok) return false;
    }
    if (p->limits.mem_bytes && (!c->limits.mem_bytes || c->limits.mem_bytes > p->limits.mem_bytes)) return false;
    if (p->limits.cpu_ms && (!c->limits.cpu_ms || c->limits.cpu_ms > p->limits.cpu_ms)) return false;
    if (p->limits.deadline_ms && (!c->limits.deadline_ms || c->limits.deadline_ms > p->limits.deadline_ms)) return false;
    return true;
}

/* ---------------------------------------------------------------- audit */
#define AUDIT_MAX 64
static struct audit_entry audit[AUDIT_MAX];
static u32 audit_head, audit_n;

static void audit_add(u32 pid, u8 kind, bool allowed, const char *res)
{
    struct audit_entry *e = &audit[audit_head];
    e->tick = pit_ticks();
    e->pid = pid;
    e->kind = kind;
    e->allowed = allowed;
    strlcpy(e->resource, res ? res : "", sizeof e->resource);
    audit_head = (audit_head + 1) % AUDIT_MAX;
    if (audit_n < AUDIT_MAX) audit_n++;
}

u32 audit_count(void) { return audit_n; }

const struct audit_entry *audit_get(u32 i)
{
    if (i >= audit_n) return NULL;
    u32 first = (audit_head + AUDIT_MAX - audit_n) % AUDIT_MAX;
    return &audit[(first + i) % AUDIT_MAX];
}

bool cap_check(struct task *t, enum cap_kind kind, const char *resource)
{
    const struct capset *cs = &t->caps;
    for (u32 i = 0; i < cs->n; i++) {
        if (cs->caps[i].kind != kind) continue;
        if (!resource || cs->caps[i].pattern[0] == 0 || cap_glob(cs->caps[i].pattern, resource))
            return true;
    }
    audit_add(t->id, (u8)kind, false, resource);
    return false;
}
