/* AO-OS syscall-interfesz. Ezt a fejlecet a kernel es a user-programok is hasznaljak.
 * Hivas: rax = szam, rdi rsi rdx r10 r8 = argumentumok, `syscall`, rax = eredmeny (<0 hiba). */
#pragma once
#include "types.h"

enum {
    SYS_EXIT = 0,     /* (code) */
    SYS_WRITE,        /* (fd, buf, n) -> irt bajtok */
    SYS_READ,         /* (fd, buf, n) -> olvasott bajtok, 0 = vege */
    SYS_OPEN,         /* (path, flags) -> fd */
    SYS_CLOSE,        /* (fd) */
    SYS_SPAWN,        /* (path, argv, manifest) -> pid */
    SYS_WAIT,         /* (pid, *status) -> pid */
    SYS_KILL,         /* (pid) */
    SYS_YIELD,        /* () */
    SYS_SLEEP,        /* (ms) */
    SYS_GETCAPS,      /* (buf, n) -> szoveges lista hossza */
    SYS_SYSINFO,      /* (struct sysinfo*) */
    SYS_LIST,         /* (path, struct dirent*, max) -> darab */
    SYS_STAT,         /* (path, struct stat*) */
    SYS_MKDIR,        /* (path) */
    SYS_UNLINK,       /* (path) */
    SYS_PIPE,         /* (int fds[2]) */
    SYS_GETPID,       /* () */
    SYS_TICKS,        /* () -> 10 ms tickek */
    SYS_SEEK,         /* (fd, off) */
    SYS_CHDIR,        /* (path) */
    SYS_GETCWD,       /* (buf, n) */
    SYS_NET_CONNECT,  /* ("ip:port") -> fd; read/write/close a socketen */
    SYS_NET_INFO,     /* (struct netinfo*) */
    SYS_MAX
};

/* hibakodok */
#define E_OK       0
#define E_INVAL   -1
#define E_NOENT   -2
#define E_CAP     -3
#define E_NOMEM   -4
#define E_BADF    -5
#define E_CHILD   -6
#define E_PIPE    -7
#define E_NOSYS   -8
#define E_TIMEOUT -9
#define E_EXIST   -10
#define E_NOTDIR  -11
#define E_ISDIR   -12
#define E_LIMIT   -13
#define E_IO      -14
#define E_ROFS    -15
#define E_BUSY    -16
#define E_FAULT   -17   /* a task CPU-kivetellel (page fault, GP...) allt le */

/* open flags */
#define O_READ   1
#define O_WRITE  2
#define O_CREATE 4
#define O_TRUNC  8
#define O_APPEND 16

struct sysinfo {
    u64 mem_total;      /* bajt */
    u64 mem_free;
    u64 ticks;
    u64 tsc_hz;
    u32 ntasks;
    u32 pid;
    char version[16];
    u32 con_cols, con_rows;     /* konzol merete cellakban */
};

#define NAME_MAX 63
struct dirent {
    char name[NAME_MAX + 1];
    u32 type;           /* 1 fajl, 2 konyvtar */
    u32 size;
};

struct stat {
    u32 type;
    u32 size;
    u64 mtime;
};

struct netinfo {
    u32 ip, mask, gw;       /* host-sorrend */
    u8  mac[6];
    u8  up, configured;
};

/* task-limitek a manifestben / spawn-nal */
struct task_limits {
    u64 mem_bytes;      /* 0 = nincs korlat */
    u64 cpu_ms;
    u64 deadline_ms;
};
