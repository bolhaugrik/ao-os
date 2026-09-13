/* AOX programok konyvtara: syscall-wrapperek + minimalis formazas. Nincs libc. */
#pragma once
#include "../kernel/include/types.h"
#include "../kernel/include/syscall.h"

i64  ao_syscall(u64 nr, u64 a, u64 b, u64 c, u64 d);

NORETURN void ao_exit(int code);
isize ao_write(int fd, const void *buf, usize n);
isize ao_read(int fd, void *buf, usize n);
int   ao_open(const char *path, u32 flags);
int   ao_close(int fd);
int   ao_spawn(const char *path, char *const *argv, const char *manifest);
int   ao_wait(int pid, int *status);
int   ao_kill(int pid);
void  ao_yield(void);
void  ao_sleep(u32 ms);
int   ao_getcaps(char *buf, usize n);
int   ao_sysinfo(struct sysinfo *si);
int   ao_list(const char *path, struct dirent *out, u32 max);
int   ao_stat(const char *path, struct stat *st);
int   ao_mkdir(const char *path);
int   ao_unlink(const char *path);
int   ao_pipe(int fds[2]);
int   ao_getpid(void);
u64   ao_ticks(void);
int   ao_net_connect(const char *addr);   /* "ip:port" -> fd */
int   ao_net_info(struct netinfo *ni);
int   ao_con_mode(u32 mode);              /* CON_TEXT / CON_RAW [| CON_NONBLOCK]; read(0) ekkor struct key_ev-eket ad */
int   ao_fb_map(struct fbinfo *fi);       /* fb capability: a kepernyo pixelei fi->vaddr-tol; kilepesig a konzol szunetel */
void *ao_sbrk(isize delta);              /* a heap vege; delta > 0 novel (nullazott lapok); NULL, ha nincs memoria/korlat */
int   ao_fb_release(void);                /* a konzol visszarajzol; ujabb ao_fb_map-ig a program ne rajzoljon */

void  ao_puts(const char *s);
int   ao_printf(const char *fmt, ...);
int   ao_getc(void);                  /* egy bajt a konzolrol (blokkol) */
usize ao_gets(char *buf, usize cap);  /* egy sor, echo-val */
const char *ao_errstr(int e);

usize strlen(const char *s);
int   strcmp(const char *a, const char *b);
void *memcpy(void *d, const void *s, usize n);
void *memset(void *d, int c, usize n);
int   memcmp(const void *a, const void *b, usize n);
