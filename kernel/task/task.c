/* Taskok es utemezo. Egy task = egy cimter + egy szal + capability-keszlet + limitek.
 * Round-robin, 100 Hz tick, ket "prioritas" helyett egyszeru sor: a kernel-szalak
 * (shell) csak onkent adjak at a vezerlest (blokkolas), a user-taskok preemptivek.
 * A kernel nem preemptalhato: IRQ-bol csak ring 3-bol vagy az idle-bol valtunk. */
#include "task.h"
#include "pipe.h"
#include "aox.h"
#include "layout.h"
#include "../arch/io.h"
#include "../cpu/gdt.h"
#include "../cpu/idt.h"
#include "../cpu/pit.h"
#include "../cpu/panic.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../mm/kheap.h"
#include "../fs/vfs.h"
#include "../net/tcp.h"
#include "../lib/string.h"

static struct task tasks[TASK_MAX];
static struct task *current;
static bool need_resched;
static struct waitq sleep_q;
static u32 next_pid = 1;
u64 current_kstack_top;

extern void switch_to(u64 *old_rsp, u64 new_rsp);
extern void task_trampoline(void);
extern void syscall_entry(void);

#define MSR_EFER  0xC0000080
#define MSR_STAR  0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_SFMASK 0xC0000084

struct task *task_current(void) { return current; }

struct task *task_get(u32 pid)
{
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != T_FREE && tasks[i].id == pid)
            return &tasks[i];
    return NULL;
}

u32 task_count(void)
{
    u32 n = 0;
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != T_FREE)
            n++;
    return n;
}

static struct task *alloc_slot(const char *name)
{
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == T_FREE) {
            struct task *t = &tasks[i];
            memset(t, 0, sizeof *t);
            t->id = next_pid++;
            strlcpy(t->name, name, sizeof t->name);
            t->kstack = kmalloc(KSTACK_SIZE);
            t->kstack_top = (u64)(uptr)t->kstack + KSTACK_SIZE;
            t->start_tick = pit_ticks();
            t->quantum_left = QUANTUM_TICKS;
            strcpy(t->cwd, "/");
            return t;
        }
    }
    return NULL;
}

/* kezdeti kernel-verem: megszakitas-keret + switch_to-keret, hogy az elso futas
 * a task_trampoline-on at iretq-val induljon */
static void build_frame(struct task *t, u64 rip, u64 cs, u64 ss, u64 rsp, u64 rdi, u64 rsi)
{
    u64 *sp = (u64 *)(uptr)t->kstack_top;
    *--sp = ss;
    *--sp = rsp;
    *--sp = 0x202;              /* rflags: IF */
    *--sp = cs;
    *--sp = rip;
    *--sp = 0;                  /* err */
    *--sp = 0;                  /* vektor */
    *--sp = 0;                  /* rax */
    *--sp = 0;                  /* rbx */
    *--sp = 0;                  /* rcx */
    *--sp = 0;                  /* rdx */
    *--sp = rsi;
    *--sp = rdi;
    *--sp = 0;                  /* rbp */
    for (int i = 0; i < 8; i++) /* r8..r15 */
        *--sp = 0;
    *--sp = (u64)(uptr)task_trampoline;
    for (int i = 0; i < 6; i++) /* rbp rbx r12-r15 a switch_to-nak */
        *--sp = 0;
    t->rsp = (u64)(uptr)sp;
}

static void kthread_wrapper(void (*fn)(void *), void *arg)
{
    sti();
    fn(arg);
    task_exit(0);
}

void task_init(void)
{
    memset(tasks, 0, sizeof tasks);
    struct task *idle = &tasks[0];
    idle->id = 0;
    strcpy(idle->name, "idle");
    idle->state = T_RUNNING;
    idle->pml4 = vmm_boot_pml4();
    capset_root(&idle->caps);
    strcpy(idle->cwd, "/");
    current = idle;

    /* syscall/sysret */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);
    /* SYSRET: CS = STAR[63:48]+16, SS = STAR[63:48]+8 -> 0x13-bol 0x23 (ucode|3) es 0x1B (udata|3) */
    wrmsr(MSR_STAR, ((u64)(SEL_KDATA | 3) << 48) | ((u64)SEL_KCODE << 32));
    wrmsr(MSR_LSTAR, (u64)(uptr)syscall_entry);
    wrmsr(MSR_SFMASK, 0x700);   /* IF, TF, DF torolve belepeskor */
}

struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg)
{
    struct task *t = alloc_slot(name);
    if (!t)
        return NULL;
    t->user = false;
    t->pml4 = vmm_boot_pml4();
    t->parent = current;
    capset_root(&t->caps);
    t->handles[0].type = H_CON_IN;
    t->handles[1].type = H_CON_OUT;
    t->handles[2].type = H_CON_OUT;
    build_frame(t, (u64)(uptr)kthread_wrapper, SEL_KCODE, SEL_KDATA, t->kstack_top - 8,
                (u64)(uptr)fn, (u64)(uptr)arg);
    t->state = T_READY;
    return t;
}

static bool map_user_pages(struct task *t, u64 vaddr, usize size)
{
    u64 limit = t->caps.limits.mem_bytes;
    for (u64 off = 0; off < size; off += PAGE_SIZE) {
        if (limit && t->mem_used + PAGE_SIZE > limit)
            return false;
        if (!vmm_map_new(t->pml4, vaddr + off, PTE_W | PTE_U))
            return false;
        t->mem_used += PAGE_SIZE;
    }
    return true;
}

static void free_task(struct task *t)
{
    if (t->kstack)
        kfree(t->kstack);
    memset(t, 0, sizeof *t);
    t->state = T_FREE;
}

struct task *task_create_user(const char *name, const void *image, usize image_size,
                              int argc, char *const *argv, const struct capset *caps,
                              struct task *parent, int *err)
{
    const struct aox_header *h = image;
    *err = E_INVAL;
    if (image_size < sizeof *h || h->magic != AOX_MAGIC || h->entry >= h->load_size || h->load_size > 16 * MiB)
        return NULL;
    struct task *t = alloc_slot(name);
    if (!t) { *err = E_LIMIT; return NULL; }
    t->user = true;
    t->parent = parent;
    t->caps = *caps;
    if (parent)
        strlcpy(t->cwd, parent->cwd, sizeof t->cwd);
    t->pml4 = vmm_new_space();
    if (!t->pml4) { free_task(t); *err = E_NOMEM; return NULL; }

    /* kep + bss */
    usize img_bytes = (h->load_size + h->bss_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    t->user_lo = USER_LOAD;
    t->user_hi = USER_LOAD + img_bytes;
    if (!map_user_pages(t, USER_LOAD, img_bytes)) goto nomem;
    usize have = image_size < h->load_size ? image_size : h->load_size;
    for (usize off = 0; off < have; off += PAGE_SIZE) {
        u64 pa = vmm_lookup(t->pml4, USER_LOAD + off);
        usize n = have - off < PAGE_SIZE ? have - off : PAGE_SIZE;
        memcpy(P2V(pa), (const u8 *)image + off, n);
    }

    /* verem: a fejlec kerese, min 16 KiB, max 1 MiB; a legfelso lap az argv-e */
    usize stack = h->stack_size ? h->stack_size : 16 * KiB;
    if (stack < 16 * KiB) stack = 16 * KiB;
    if (stack > 1 * MiB) stack = 1 * MiB;
    stack = (stack + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    t->stack_hi = USER_STACK_TOP;
    t->stack_lo = USER_STACK_TOP - stack;
    if (!map_user_pages(t, t->stack_lo, stack)) goto nomem;

    /* argv a verem tetejen: stringek, majd a mutato-tomb; a belepeskor rsp alatta */
    u64 top_page_pa = vmm_lookup(t->pml4, USER_STACK_TOP - PAGE_SIZE);
    u8 *pg = P2V(top_page_pa);
    u64 user_pg = USER_STACK_TOP - PAGE_SIZE;
    usize off = PAGE_SIZE;
    u64 ptrs[16];
    if (argc > 16) argc = 16;
    for (int i = argc - 1; i >= 0; i--) {
        usize len = strlen(argv[i]) + 1;
        if (off < len + 256) { argc = i; break; }
        off -= len;
        memcpy(pg + off, argv[i], len);
        ptrs[i] = user_pg + off;
    }
    off &= ~15ULL;
    off -= (usize)(argc + 1) * 8;
    off &= ~15ULL;
    u64 *uargv = (u64 *)(pg + off);
    for (int i = 0; i < argc; i++)
        uargv[i] = ptrs[i];
    uargv[argc] = 0;
    u64 user_rsp = user_pg + off - 8;     /* belepeskor rsp = 16n + 8 */

    for (int i = 0; i < HANDLE_MAX; i++) t->handles[i].type = H_NONE;
    t->handles[0].type = H_CON_IN;
    t->handles[1].type = H_CON_OUT;
    t->handles[2].type = H_CON_OUT;

    if (t->caps.limits.deadline_ms)
        t->deadline_tick = pit_ticks() + t->caps.limits.deadline_ms / 10 + 1;

    build_frame(t, USER_LOAD + h->entry, SEL_USER_CS, SEL_USER_SS, user_rsp, (u64)argc, user_pg + off);
    t->state = T_READY;
    *err = 0;
    return t;

nomem:
    vmm_destroy_space(t->pml4);
    free_task(t);
    *err = E_NOMEM;
    return NULL;
}

/* ---------------------------------------------------------------- varakozasi sorok */
void waitq_wake_all(struct waitq *q)
{
    struct task *t = q->head;
    q->head = NULL;
    while (t) {
        struct task *n = t->wq_next;
        t->wq = NULL;
        t->wq_next = NULL;
        if (t->state == T_BLOCKED)
            t->state = T_READY;
        t = n;
    }
}

static void waitq_remove(struct task *t)
{
    struct waitq *q = t->wq;
    if (!q) return;
    struct task **pp = &q->head;
    while (*pp) {
        if (*pp == t) { *pp = t->wq_next; break; }
        pp = &(*pp)->wq_next;
    }
    t->wq = NULL;
    t->wq_next = NULL;
}

void task_block_on(struct waitq *q)
{
    cli();
    struct task *t = current;
    t->state = T_BLOCKED;
    t->wq = q;
    t->wq_next = q->head;
    q->head = t;
    schedule();
    sti();
}

/* ---------------------------------------------------------------- utemezo */
static struct task *pick_next(void)
{
    int start = (int)(current - tasks);
    for (int k = 1; k <= TASK_MAX; k++) {
        int i = (start + k) % TASK_MAX;
        if (i == 0) continue;
        if (tasks[i].state == T_READY)
            return &tasks[i];
    }
    return NULL;
}

void schedule(void)
{
    struct task *prev = current;
    struct task *next = pick_next();
    if (!next) {
        if (prev->state == T_RUNNING)
            return;                     /* fut tovabb (pl. idle vagy egyetlen task) */
        next = &tasks[0];               /* idle */
    }
    if (next == prev) {
        prev->state = T_RUNNING;
        return;
    }
    if (prev->state == T_RUNNING)
        prev->state = T_READY;
    next->state = T_RUNNING;
    next->quantum_left = QUANTUM_TICKS;
    current = next;
    current_kstack_top = next->kstack_top;
    gdt_set_kernel_stack(next->kstack_top);
    if (next->pml4 != prev->pml4)
        vmm_switch(next->pml4);
    switch_to(&prev->rsp, next->rsp);
}

void task_yield(void)
{
    cli();
    current->state = T_READY;
    schedule();
    sti();
}

void task_sleep_ms(u32 ms)
{
    cli();
    current->wake_tick = pit_ticks() + ms / 10 + 1;
    sti();
    task_block_on(&sleep_q);
}

void task_tick(void)
{
    u64 now = pit_ticks();
    /* alvok ebresztese */
    for (int i = 1; i < TASK_MAX; i++) {
        struct task *t = &tasks[i];
        if (t->state == T_BLOCKED && t->wq == &sleep_q && t->wake_tick <= now) {
            waitq_remove(t);
            t->state = T_READY;
        }
    }
    if (current->user) {
        current->cpu_ticks++;
        u64 cpu_ms = current->caps.limits.cpu_ms;
        if (cpu_ms && current->cpu_ticks * 10 >= cpu_ms)
            current->killed = true;
        if (current->deadline_tick && now >= current->deadline_tick)
            current->killed = true;
        if (current->quantum_left && --current->quantum_left == 0)
            need_resched = true;
    }
    /* hatarido a nem futo taskokra is */
    for (int i = 1; i < TASK_MAX; i++) {
        struct task *t = &tasks[i];
        if (t->user && t->state != T_ZOMBIE && t->deadline_tick && now >= t->deadline_tick && !t->killed)
            task_kill(t->id);
    }
    if (pick_next() && current == &tasks[0])
        need_resched = true;
}

void task_preempt_check(struct regs *r)
{
    bool from_user = (r->cs & 3) != 0;
    if (from_user && current->killed)
        task_exit(E_TIMEOUT);
    if (need_resched && (from_user || current == &tasks[0])) {
        need_resched = false;
        schedule();
    }
}

NORETURN void task_idle_loop(void)
{
    for (;;) {
        cli();
        schedule();
        idle_enter();
    }
}

/* ---------------------------------------------------------------- vege, varas, kill */
static void close_handle(struct handle *h)
{
    if (h->type == H_PIPE_R) pipe_close(h->obj, false);
    else if (h->type == H_PIPE_W) pipe_close(h->obj, true);
    else if (h->type == H_SOCK) tcp_close((int)(uptr)h->obj);
    else if (h->type == H_FILE || h->type == H_DIR) vfs_close(h);
    h->type = H_NONE;
    h->obj = NULL;
}

NORETURN void task_exit(int code)
{
    cli();
    struct task *t = current;
    if (t == &tasks[0])
        panic("az idle task nem lephet ki");
    for (int i = 0; i < HANDLE_MAX; i++)
        close_handle(&t->handles[i]);
    if (t->user) {
        vmm_switch(vmm_boot_pml4());
        vmm_destroy_space(t->pml4);
        t->pml4 = vmm_boot_pml4();
    }
    t->exit_code = code;
    t->state = T_ZOMBIE;
    /* arva gyerekek: a szulojuk eltunik */
    for (int i = 1; i < TASK_MAX; i++)
        if (tasks[i].parent == t)
            tasks[i].parent = NULL;
    waitq_wake_all(&t->exit_q);
    schedule();
    for (;;)
        hlt();
}

int task_wait(u32 pid, int *status)
{
    for (;;) {
        cli();
        struct task *child = NULL;
        bool any = false;
        for (int i = 1; i < TASK_MAX; i++) {
            struct task *t = &tasks[i];
            if (t->state == T_FREE || t->parent != current) continue;
            if (pid && t->id != pid) continue;
            any = true;
            if (t->state == T_ZOMBIE) { child = t; break; }
        }
        if (!any) { sti(); return E_CHILD; }
        if (child) {
            u32 id = child->id;
            if (status) *status = child->exit_code;
            free_task(child);
            sti();
            return (int)id;
        }
        /* varunk barmelyik gyerekre: a konkret gyerek exit_q-jara, vagy az elsore */
        struct task *w = NULL;
        for (int i = 1; i < TASK_MAX; i++)
            if (tasks[i].state != T_FREE && tasks[i].parent == current && (!pid || tasks[i].id == pid)) { w = &tasks[i]; break; }
        task_block_on(&w->exit_q);   /* IF-et visszakapcsolja */
    }
}

bool task_kill(u32 pid)
{
    struct task *t = task_get(pid);
    if (!t || t == &tasks[0] || !t->user)
        return false;
    t->killed = true;
    if (t->state == T_BLOCKED) {
        waitq_remove(t);
        t->state = T_READY;
    }
    return true;
}

/* zombik, akiknek nincs szuloje: a ps takaritja */
void task_reap_orphans(void)
{
    cli();
    for (int i = 1; i < TASK_MAX; i++)
        if (tasks[i].state == T_ZOMBIE && tasks[i].parent == NULL)
            free_task(&tasks[i]);
    sti();
}

const struct task *task_at(int i) { return (i >= 0 && i < TASK_MAX) ? &tasks[i] : NULL; }
