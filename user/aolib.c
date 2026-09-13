#include "aolib.h"
#include "../kernel/lib/fmt.h"

i64 ao_syscall(u64 nr, u64 a, u64 b, u64 c, u64 d)
{
    i64 ret;
    register u64 r10 __asm__("r10") = d;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

NORETURN void ao_exit(int code)
{
    ao_syscall(SYS_EXIT, (u64)(i64)code, 0, 0, 0);
    for (;;) ;
}

isize ao_write(int fd, const void *buf, usize n) { return (isize)ao_syscall(SYS_WRITE, (u64)fd, (u64)(uptr)buf, n, 0); }
isize ao_read(int fd, void *buf, usize n) { return (isize)ao_syscall(SYS_READ, (u64)fd, (u64)(uptr)buf, n, 0); }
int ao_open(const char *path, u32 flags) { return (int)ao_syscall(SYS_OPEN, (u64)(uptr)path, flags, 0, 0); }
int ao_close(int fd) { return (int)ao_syscall(SYS_CLOSE, (u64)fd, 0, 0, 0); }
int ao_spawn(const char *path, char *const *argv, const char *manifest)
{ return (int)ao_syscall(SYS_SPAWN, (u64)(uptr)path, (u64)(uptr)argv, (u64)(uptr)manifest, 0); }
int ao_wait(int pid, int *status) { return (int)ao_syscall(SYS_WAIT, (u64)pid, (u64)(uptr)status, 0, 0); }
int ao_kill(int pid) { return (int)ao_syscall(SYS_KILL, (u64)pid, 0, 0, 0); }
void ao_yield(void) { ao_syscall(SYS_YIELD, 0, 0, 0, 0); }
void ao_sleep(u32 ms) { ao_syscall(SYS_SLEEP, ms, 0, 0, 0); }
int ao_getcaps(char *buf, usize n) { return (int)ao_syscall(SYS_GETCAPS, (u64)(uptr)buf, n, 0, 0); }
int ao_sysinfo(struct sysinfo *si) { return (int)ao_syscall(SYS_SYSINFO, (u64)(uptr)si, 0, 0, 0); }
int ao_list(const char *path, struct dirent *out, u32 max)
{ return (int)ao_syscall(SYS_LIST, (u64)(uptr)path, (u64)(uptr)out, max, 0); }
int ao_stat(const char *path, struct stat *st) { return (int)ao_syscall(SYS_STAT, (u64)(uptr)path, (u64)(uptr)st, 0, 0); }
int ao_mkdir(const char *path) { return (int)ao_syscall(SYS_MKDIR, (u64)(uptr)path, 0, 0, 0); }
int ao_unlink(const char *path) { return (int)ao_syscall(SYS_UNLINK, (u64)(uptr)path, 0, 0, 0); }
int ao_pipe(int fds[2]) { return (int)ao_syscall(SYS_PIPE, (u64)(uptr)fds, 0, 0, 0); }
int ao_getpid(void) { return (int)ao_syscall(SYS_GETPID, 0, 0, 0, 0); }
u64 ao_ticks(void) { return (u64)ao_syscall(SYS_TICKS, 0, 0, 0, 0); }
int ao_net_connect(const char *addr) { return (int)ao_syscall(SYS_NET_CONNECT, (u64)(uptr)addr, 0, 0, 0); }
int ao_net_info(struct netinfo *ni) { return (int)ao_syscall(SYS_NET_INFO, (u64)(uptr)ni, 0, 0, 0); }
int ao_con_mode(u32 mode) { return (int)ao_syscall(SYS_CON_MODE, mode, 0, 0, 0); }
int ao_fb_map(struct fbinfo *fi) { return (int)ao_syscall(SYS_FB_MAP, (u64)(uptr)fi, 0, 0, 0); }

void ao_puts(const char *s) { ao_write(1, s, strlen(s)); }

struct pbuf { char b[256]; usize n; };
static void pout(char c, void *ctx)
{
    struct pbuf *p = ctx;
    p->b[p->n++] = c;
    if (p->n == sizeof p->b) { ao_write(1, p->b, p->n); p->n = 0; }
}

int ao_printf(const char *fmt, ...)
{
    struct pbuf p = { .n = 0 };
    va_list ap;
    va_start(ap, fmt);
    vformat(pout, &p, fmt, ap);
    va_end(ap);
    if (p.n) ao_write(1, p.b, p.n);
    return 0;
}

int ao_getc(void)
{
    u8 c;
    isize r = ao_read(0, &c, 1);
    return r == 1 ? c : (int)r;
}

usize ao_gets(char *buf, usize cap)
{
    usize n = 0;
    for (;;) {
        int c = ao_getc();
        if (c < 0) break;
        if (c == '\n') { ao_puts("\n"); break; }
        if (c == '\b') { if (n) { n--; ao_puts("\b"); } continue; }
        if (c < 32 || n + 1 >= cap) continue;
        buf[n++] = (char)c;
        ao_write(1, &c, 1);
    }
    buf[n] = 0;
    return n;
}

const char *ao_errstr(int e)
{
    switch (e) {
    case E_OK: return "ok";
    case E_INVAL: return "E_INVAL";
    case E_NOENT: return "E_NOENT";
    case E_CAP: return "E_CAP";
    case E_NOMEM: return "E_NOMEM";
    case E_BADF: return "E_BADF";
    case E_CHILD: return "E_CHILD";
    case E_PIPE: return "E_PIPE";
    case E_NOSYS: return "E_NOSYS";
    case E_TIMEOUT: return "E_TIMEOUT";
    case E_EXIST: return "E_EXIST";
    case E_NOTDIR: return "E_NOTDIR";
    case E_ISDIR: return "E_ISDIR";
    case E_LIMIT: return "E_LIMIT";
    case E_IO: return "E_IO";
    case E_ROFS: return "E_ROFS";
    case E_BUSY: return "E_BUSY";
    case E_FAULT: return "E_FAULT";
    default: return "?";
    }
}
