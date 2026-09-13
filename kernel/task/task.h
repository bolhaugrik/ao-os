#pragma once
#include "types.h"
#include "syscall.h"
#include "../cap/cap.h"

struct regs;

#define TASK_MAX    32
#define KSTACK_SIZE (32 * 1024)     /* az AOFS2 4 KiB-os blokk-puffereket tart a vermen */
#define HANDLE_MAX  16
#define USER_LOAD   0x400000ULL
#define USER_STACK_TOP 0x00007FFF00000000ULL
#define QUANTUM_TICKS 2

enum task_state { T_FREE = 0, T_READY, T_RUNNING, T_BLOCKED, T_ZOMBIE };

enum handle_type { H_NONE = 0, H_CON_IN, H_CON_OUT, H_PIPE_R, H_PIPE_W, H_FILE, H_DIR, H_SOCK };

struct handle {
    u8  type;
    u8  flags;
    u16 pad;
    u32 pad2;
    void *obj;
    u64 pos;
};

struct waitq {
    struct task *head;
};

struct task {
    u32 id;
    enum task_state state;
    char name[32];
    u32 con_mode;               /* CON_* (nyers billentyu-esemenyek); kilepeskor visszaall */
    bool fb_mapped;             /* a framebuffer a cimterben van (fb capability); kilepeskor a konzol visszajon */
    bool user;
    bool killed;
    u64 pml4;
    u8 *kstack;
    u64 kstack_top;
    u64 rsp;                    /* mentett kernel-verem a kontextusvaltashoz */
    u64 user_lo, user_hi;       /* kep + bss */
    u64 stack_lo, stack_hi;
    struct task *parent;
    int exit_code;
    struct waitq exit_q;        /* a szulo var itt */
    struct task *wq_next;
    struct waitq *wq;
    u64 wake_tick;
    struct handle handles[HANDLE_MAX];
    struct capset caps;
    char cwd[128];
    u64 mem_used;               /* user lapok */
    u64 cpu_ticks;
    u64 deadline_tick;          /* 0 = nincs */
    u64 quantum_left;
    u64 start_tick;
};

void task_init(void);                          /* a boot-kontextus = 0. task (idle) */
struct task *task_current(void);
struct task *task_get(u32 pid);
u32  task_count(void);
struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg);
struct task *task_create_user(const char *name, const void *image, usize image_size,
                              int argc, char *const *argv, const struct capset *caps,
                              struct task *parent, int *err);
NORETURN void task_exit(int code);
int  task_wait(u32 pid, int *status);          /* blokkol; visszaadja a pid-et vagy hibat */
bool task_kill(u32 pid);
void task_yield(void);
void task_sleep_ms(u32 ms);
void task_block_on(struct waitq *q);           /* IF barmilyen; visszateres utan IF=1 */
bool task_block_timeout(struct waitq *q, u32 ms); /* false = lejart az ido (nem ebresztettek) */
void waitq_wake_all(struct waitq *q);
void schedule(void);                           /* IF=0 mellett hivando */
void task_tick(void);                          /* PIT-bol */
void task_preempt_check(struct regs *r);       /* IRQ vege, ring 3 visszateres elott */
NORETURN void task_idle_loop(void);

void task_reap_orphans(void);
const struct task *task_at(int i);             /* slot szerinti bejaras (ps) */

extern u64 current_kstack_top;                 /* a syscall-belepes hasznalja */
struct regs;
