/* Syscall-elosztas. Minden eroforrast megnevezo hivas a cap_check-en megy at. */
#include "task.h"
#include "pipe.h"
#include "syscall.h"
#include "layout.h"
#include "../cpu/idt.h"
#include "../cpu/pit.h"
#include "../cpu/tsc.h"
#include "../drv/console.h"
#include "../drv/kbd.h"
#include "../fs/vfs.h"
#include "../net/net.h"
#include "../net/tcp.h"
#include "../mm/pmm.h"
#include "../mm/kheap.h"
#include "../lib/string.h"
#include "../lib/fmt.h"

#define AO_VERSION_STR "0.1-phase2"

/* ---------------------------------------------------------------- user-mutatok */
static bool user_range_ok(const struct task *t, u64 p, u64 n)
{
    if (!t->user)
        return true;
    if (n == 0) return true;
    if (p + n < p) return false;
    if (p >= t->user_lo && p + n <= t->user_hi) return true;
    if (p >= t->stack_lo && p + n <= t->stack_hi) return true;
    return false;
}

/* string masolasa a user-terbol; hossz-korlattal, ellenorzott cimekkel */
static bool copy_str(const struct task *t, u64 up, char *dst, usize cap)
{
    for (usize i = 0; i < cap; i++) {
        if (!user_range_ok(t, up + i, 1))
            return false;
        dst[i] = *(const char *)(uptr)(up + i);
        if (!dst[i])
            return true;
    }
    return false;
}

/* ---------------------------------------------------------------- konzol */
static isize con_read(u8 *buf, usize n)
{
    if (n == 0) return 0;
    struct key_event ev;
    kbd_wait(&ev);
    if (task_current()->killed)
        return E_TIMEOUT;
    u16 k = ev.code;
    if (k < 0x100) { buf[0] = (u8)k; return 1; }
    const char *seq = NULL;
    switch (k) {
    case KEY_UP: seq = "\x1b[A"; break;
    case KEY_DOWN: seq = "\x1b[B"; break;
    case KEY_RIGHT: seq = "\x1b[C"; break;
    case KEY_LEFT: seq = "\x1b[D"; break;
    case KEY_HOME: seq = "\x1b[H"; break;
    case KEY_END: seq = "\x1b[F"; break;
    case KEY_DEL: seq = "\x1b[3~"; break;
    case KEY_PGUP: seq = "\x1b[5~"; break;
    case KEY_PGDN: seq = "\x1b[6~"; break;
    case KEY_ESC: seq = "\x1b"; break;
    default: return 0;
    }
    usize l = strlen(seq);
    if (l > n) l = n;
    memcpy(buf, seq, l);
    return (isize)l;
}

/* ---------------------------------------------------------------- handle-ek */
static int alloc_fd(struct task *t)
{
    for (int i = 0; i < HANDLE_MAX; i++)
        if (t->handles[i].type == H_NONE)
            return i;
    return E_LIMIT;
}

static isize sys_write(struct task *t, int fd, u64 ubuf, usize n)
{
    if (fd < 0 || fd >= HANDLE_MAX || t->handles[fd].type == H_NONE) return E_BADF;
    if (!user_range_ok(t, ubuf, n)) return E_INVAL;
    struct handle *h = &t->handles[fd];
    const u8 *buf = (const u8 *)(uptr)ubuf;
    switch (h->type) {
    case H_CON_OUT:
        if (!cap_check(t, CAP_CONSOLE, NULL)) return E_CAP;
        for (usize i = 0; i < n; i++) console_putc((char)buf[i]);
        console_flush();
        return (isize)n;
    case H_PIPE_W:
        return pipe_write(h->obj, buf, n);
    case H_FILE:
        return vfs_write(h, buf, n);
    case H_SOCK:
        return tcp_send((int)(uptr)h->obj, buf, n);
    default:
        return E_BADF;
    }
}

static isize sys_read(struct task *t, int fd, u64 ubuf, usize n)
{
    if (fd < 0 || fd >= HANDLE_MAX || t->handles[fd].type == H_NONE) return E_BADF;
    if (!user_range_ok(t, ubuf, n)) return E_INVAL;
    struct handle *h = &t->handles[fd];
    u8 *buf = (u8 *)(uptr)ubuf;
    switch (h->type) {
    case H_CON_IN:
        if (!cap_check(t, CAP_CONSOLE, NULL)) return E_CAP;
        return con_read(buf, n);
    case H_PIPE_R:
        return pipe_read(h->obj, buf, n);
    case H_FILE:
        return vfs_read(h, buf, n);
    case H_SOCK:
        return tcp_recv((int)(uptr)h->obj, buf, n, 0);
    default:
        return E_BADF;
    }
}

static int sys_close(struct task *t, int fd)
{
    if (fd < 0 || fd >= HANDLE_MAX || t->handles[fd].type == H_NONE) return E_BADF;
    struct handle *h = &t->handles[fd];
    if (h->type == H_PIPE_R) pipe_close(h->obj, false);
    else if (h->type == H_PIPE_W) pipe_close(h->obj, true);
    else if (h->type == H_FILE || h->type == H_DIR) vfs_close(h);
    else if (h->type == H_SOCK) tcp_close((int)(uptr)h->obj);
    h->type = H_NONE;
    h->obj = NULL;
    return 0;
}

static int sys_pipe(struct task *t, u64 ufds)
{
    if (!user_range_ok(t, ufds, 8)) return E_INVAL;
    int r = alloc_fd(t);
    if (r < 0) return r;
    t->handles[r].type = H_PIPE_R;
    int w = alloc_fd(t);
    if (w < 0) { t->handles[r].type = H_NONE; return w; }
    struct pipe *p = pipe_create();
    t->handles[r].obj = p;
    t->handles[w].type = H_PIPE_W;
    t->handles[w].obj = p;
    int *fds = (int *)(uptr)ufds;
    fds[0] = r;
    fds[1] = w;
    return 0;
}

/* ---------------------------------------------------------------- fajlok */
static int path_arg(struct task *t, u64 up, char *out, usize cap)
{
    char raw[VFS_PATH_MAX];
    if (!copy_str(t, up, raw, sizeof raw)) return E_INVAL;
    if (!vfs_canon(t->cwd, raw, out, cap)) return E_INVAL;
    return 0;
}

static int sys_open(struct task *t, u64 upath, u32 flags)
{
    char path[VFS_PATH_MAX];
    int e = path_arg(t, upath, path, sizeof path);
    if (e) return e;
    if ((flags & (O_WRITE | O_CREATE | O_TRUNC | O_APPEND)) && !cap_check(t, CAP_FS_WRITE, path)) return E_CAP;
    if ((flags & O_READ) && !cap_check(t, CAP_FS_READ, path)) return E_CAP;
    if (!(flags & (O_READ | O_WRITE))) return E_INVAL;
    int fd = alloc_fd(t);
    if (fd < 0) return fd;
    e = vfs_open(path, flags, &t->handles[fd]);
    if (e) { t->handles[fd].type = H_NONE; return e; }
    return fd;
}

static int sys_list(struct task *t, u64 upath, u64 ubuf, u32 max)
{
    char path[VFS_PATH_MAX];
    int e = path_arg(t, upath, path, sizeof path);
    if (e) return e;
    if (!cap_check(t, CAP_FS_READ, path)) return E_CAP;
    if (max > 256) max = 256;
    if (!user_range_ok(t, ubuf, (u64)max * sizeof(struct dirent))) return E_INVAL;
    return vfs_list(path, (struct dirent *)(uptr)ubuf, max);
}

static int sys_stat(struct task *t, u64 upath, u64 ust)
{
    char path[VFS_PATH_MAX];
    int e = path_arg(t, upath, path, sizeof path);
    if (e) return e;
    if (!cap_check(t, CAP_FS_READ, path)) return E_CAP;
    if (!user_range_ok(t, ust, sizeof(struct stat))) return E_INVAL;
    return vfs_stat(path, (struct stat *)(uptr)ust);
}

static int sys_mkdir(struct task *t, u64 upath)
{
    char path[VFS_PATH_MAX];
    int e = path_arg(t, upath, path, sizeof path);
    if (e) return e;
    if (!cap_check(t, CAP_FS_WRITE, path)) return E_CAP;
    return vfs_mkdir(path);
}

static int sys_unlink(struct task *t, u64 upath)
{
    char path[VFS_PATH_MAX];
    int e = path_arg(t, upath, path, sizeof path);
    if (e) return e;
    if (!cap_check(t, CAP_FS_WRITE, path)) return E_CAP;
    return vfs_unlink(path);
}

static int sys_chdir(struct task *t, u64 upath)
{
    char path[VFS_PATH_MAX];
    int e = path_arg(t, upath, path, sizeof path);
    if (e) return e;
    if (!cap_check(t, CAP_FS_READ, path)) return E_CAP;
    struct stat st;
    e = vfs_stat(path, &st);
    if (e) return e;
    if (st.type != 2) return E_NOTDIR;
    strlcpy(t->cwd, path, sizeof t->cwd);
    return 0;
}

/* ---------------------------------------------------------------- spawn */
static int sys_spawn(struct task *t, u64 upath, u64 uargv, u64 umanifest)
{
    if (!cap_check(t, CAP_TASK_SPAWN, NULL)) return E_CAP;
    char path[VFS_PATH_MAX];
    int e = path_arg(t, upath, path, sizeof path);
    if (e) return e;
    if (!cap_check(t, CAP_EXEC, path)) return E_CAP;

    /* argv masolasa */
    char *argv[16];
    char argbuf[16][64];
    int argc = 0;
    if (uargv) {
        for (; argc < 16; argc++) {
            if (!user_range_ok(t, uargv + (u64)argc * 8, 8)) return E_INVAL;
            u64 sp = *(const u64 *)(uptr)(uargv + (u64)argc * 8);
            if (!sp) break;
            if (!copy_str(t, sp, argbuf[argc], sizeof argbuf[argc])) return E_INVAL;
            argv[argc] = argbuf[argc];
        }
    }
    if (argc == 0) { strlcpy(argbuf[0], path, sizeof argbuf[0]); argv[0] = argbuf[0]; argc = 1; }

    /* capability-keszlet: manifest szoveg vagy orokles; csak szukites */
    struct capset cs;
    if (umanifest) {
        char text[1024];
        if (!copy_str(t, umanifest, text, sizeof text)) return E_INVAL;
        char err[64];
        if (!capset_parse(&cs, text, strlen(text), err, sizeof err)) return E_INVAL;
        if (!capset_subset(&cs, &t->caps)) { cap_check(t, CAP_TASK_SPAWN, cs.name); return E_CAP; }
    } else {
        cs = t->caps;
    }

    void *img;
    usize size;
    e = vfs_read_all(path, &img, &size);
    if (e) return e;
    int err;
    struct task *c = task_create_user(argv[0], img, size, argc, argv, &cs, t, &err);
    kfree(img);
    if (!c) return err;
    return (int)c->id;
}

static int sys_getcaps(struct task *t, u64 ubuf, usize n)
{
    if (!user_range_ok(t, ubuf, n)) return E_INVAL;
    char *out = (char *)(uptr)ubuf;
    usize pos = 0;
    pos += snformat(out + pos, n > pos ? n - pos : 0, "agent %s\n", t->caps.name);
    for (u32 i = 0; i < t->caps.n; i++)
        pos += snformat(out + pos, n > pos ? n - pos : 0, "  %s %s\n",
                        cap_kind_name(t->caps.caps[i].kind), t->caps.caps[i].pattern);
    if (t->caps.limits.mem_bytes) pos += snformat(out + pos, n > pos ? n - pos : 0, "  mem %luK\n", t->caps.limits.mem_bytes / 1024);
    if (t->caps.limits.cpu_ms) pos += snformat(out + pos, n > pos ? n - pos : 0, "  cpu %lums\n", t->caps.limits.cpu_ms);
    if (t->caps.limits.deadline_ms) pos += snformat(out + pos, n > pos ? n - pos : 0, "  deadline %lums\n", t->caps.limits.deadline_ms);
    return (int)pos;
}

static int sys_sysinfo(struct task *t, u64 uptr_)
{
    if (!cap_check(t, CAP_SYS_INFO, NULL)) return E_CAP;
    if (!user_range_ok(t, uptr_, sizeof(struct sysinfo))) return E_INVAL;
    struct sysinfo *si = (struct sysinfo *)(uptr)uptr_;
    si->mem_total = pmm_total_frames() * PAGE_SIZE;
    si->mem_free = pmm_free_frames() * PAGE_SIZE;
    si->ticks = pit_ticks();
    si->tsc_hz = tsc_hz();
    si->ntasks = task_count();
    si->pid = t->id;
    strlcpy(si->version, AO_VERSION_STR, sizeof si->version);
    return 0;
}

/* ---------------------------------------------------------------- halozat */
static int sys_net_connect(struct task *t, u64 uaddr)
{
    char addr[64];
    if (!copy_str(t, uaddr, addr, sizeof addr)) return E_INVAL;
    u32 ip;
    if (!ip_parse(addr, &ip)) return E_INVAL;
    const char *p = addr;
    while (*p && *p != ':') p++;
    if (*p != ':') return E_INVAL;
    u32 port = 0;
    for (p++; *p >= '0' && *p <= '9'; p++) port = port * 10 + (u32)(*p - '0');
    if (!port || port > 65535) return E_INVAL;
    if (!cap_check(t, CAP_NET, addr)) return E_CAP;
    int fd = alloc_fd(t);
    if (fd < 0) return fd;
    int s = tcp_connect(ip, (u16)port, 5000);
    if (s < 0) return s;
    t->handles[fd].type = H_SOCK;
    t->handles[fd].obj = (void *)(uptr)s;
    t->handles[fd].pos = 0;
    return fd;
}

static int sys_net_info(struct task *t, u64 uptr_)
{
    if (!user_range_ok(t, uptr_, sizeof(struct netinfo))) return E_INVAL;
    struct netinfo *ni = (struct netinfo *)(uptr)uptr_;
    memset(ni, 0, sizeof *ni);
    ni->ip = net_cfg.ip; ni->mask = net_cfg.mask; ni->gw = net_cfg.gw;
    ni->configured = net_cfg.configured;
    if (net_dev()) { memcpy(ni->mac, net_dev()->mac, 6); ni->up = net_dev()->up; }
    return 0;
}

/* ---------------------------------------------------------------- elosztas */
void syscall_dispatch(struct regs *r)
{
    struct task *t = task_current();
    u64 nr = r->rax, a = r->rdi, b = r->rsi, c = r->rdx, d = r->r10;
    i64 ret;
    switch (nr) {
    case SYS_EXIT:    task_exit((int)a);
    case SYS_WRITE:   ret = sys_write(t, (int)a, b, c); break;
    case SYS_READ:    ret = sys_read(t, (int)a, b, c); break;
    case SYS_OPEN:    ret = sys_open(t, a, (u32)b); break;
    case SYS_CLOSE:   ret = sys_close(t, (int)a); break;
    case SYS_SPAWN:   ret = sys_spawn(t, a, b, c); break;
    case SYS_WAIT: {
        int status = 0;
        ret = task_wait((u32)a, &status);
        if (b && user_range_ok(t, b, 4)) *(int *)(uptr)b = status;
        break;
    }
    case SYS_KILL:    ret = task_kill((u32)a) ? 0 : E_NOENT; break;
    case SYS_YIELD:   task_yield(); ret = 0; break;
    case SYS_SLEEP:   task_sleep_ms((u32)a); ret = 0; break;
    case SYS_GETCAPS: ret = sys_getcaps(t, a, b); break;
    case SYS_SYSINFO: ret = sys_sysinfo(t, a); break;
    case SYS_LIST:    ret = sys_list(t, a, b, (u32)c); break;
    case SYS_STAT:    ret = sys_stat(t, a, b); break;
    case SYS_MKDIR:   ret = sys_mkdir(t, a); break;
    case SYS_UNLINK:  ret = sys_unlink(t, a); break;
    case SYS_PIPE:    ret = sys_pipe(t, a); break;
    case SYS_GETPID:  ret = t->id; break;
    case SYS_TICKS:   ret = (i64)pit_ticks(); break;
    case SYS_SEEK:
        if ((int)a < 0 || (int)a >= HANDLE_MAX || t->handles[a].type != H_FILE) ret = E_BADF;
        else { t->handles[a].pos = b; ret = (i64)b; }
        break;
    case SYS_CHDIR:   ret = sys_chdir(t, a); break;
    case SYS_GETCWD:
        if (!user_range_ok(t, a, b)) ret = E_INVAL;
        else { strlcpy((char *)(uptr)a, t->cwd, b); ret = (i64)strlen(t->cwd); }
        break;
    case SYS_NET_CONNECT: ret = sys_net_connect(t, a); break;
    case SYS_NET_INFO: ret = sys_net_info(t, a); break;
    default:          ret = E_NOSYS; break;
    }
    (void)d;
    r->rax = (u64)ret;
    if (t->killed)
        task_exit(E_TIMEOUT);
}
