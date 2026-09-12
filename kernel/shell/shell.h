#pragma once
#include "types.h"
#include "bootinfo.h"

void shell_run(const struct bootinfo *bi);   /* nem ter vissza */

/* kprintf: konzol (+ soros tukor), a shell es a kernel kozos kimenete */
int kprintf(const char *fmt, ...);
