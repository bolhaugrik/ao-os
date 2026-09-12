#include "pipe.h"
#include "../arch/io.h"
#include "../mm/kheap.h"
#include "../lib/string.h"

struct pipe *pipe_create(void)
{
    struct pipe *p = kzalloc(sizeof *p);
    p->readers = 1;
    p->writers = 1;
    return p;
}

void pipe_close(struct pipe *p, bool write_end)
{
    if (write_end) {
        if (p->writers) p->writers--;
        waitq_wake_all(&p->rq);
    } else {
        if (p->readers) p->readers--;
        waitq_wake_all(&p->wq);
    }
    if (!p->readers && !p->writers)
        kfree(p);
}

isize pipe_read(struct pipe *p, void *buf, usize n)
{
    u8 *d = buf;
    cli();
    while (p->count == 0) {
        if (!p->writers) { sti(); return 0; }
        task_block_on(&p->rq);
        if (task_current()->killed) return E_PIPE;
        cli();
    }
    sti();
    usize got = 0;
    while (got < n && p->count) {
        d[got++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF;
        p->count--;
    }
    waitq_wake_all(&p->wq);
    return (isize)got;
}

isize pipe_write(struct pipe *p, const void *buf, usize n)
{
    const u8 *s = buf;
    usize put = 0;
    while (put < n) {
        if (!p->readers)
            return put ? (isize)put : E_PIPE;
        cli();
        while (p->count == PIPE_BUF) {
            task_block_on(&p->wq);
            if (task_current()->killed || !p->readers)
                return put ? (isize)put : E_PIPE;
            cli();
        }
        sti();
        p->buf[p->head] = s[put++];
        p->head = (p->head + 1) % PIPE_BUF;
        p->count++;
        if (p->count == 1)
            waitq_wake_all(&p->rq);
    }
    waitq_wake_all(&p->rq);
    return (isize)put;
}
