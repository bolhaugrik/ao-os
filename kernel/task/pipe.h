#pragma once
#include "types.h"
#include "task.h"

#define PIPE_BUF 4096

struct pipe {
    u8  buf[PIPE_BUF];
    u32 head, tail, count;
    u32 readers, writers;
    struct waitq rq, wq;
};

struct pipe *pipe_create(void);
void pipe_close(struct pipe *p, bool write_end);
isize pipe_read(struct pipe *p, void *buf, usize n);     /* blokkol, 0 = EOF */
isize pipe_write(struct pipe *p, const void *buf, usize n);
