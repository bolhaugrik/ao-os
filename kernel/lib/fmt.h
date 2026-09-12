/* Minimalis formazo: %s %c %d %u %x %lx %llx %p %%, szelesseg es 0-kitoltes. */
#pragma once
#include "types.h"

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v, t)   __builtin_va_arg(v, t)

typedef void (*fmt_out_fn)(char c, void *ctx);

void vformat(fmt_out_fn out, void *ctx, const char *fmt, va_list ap);
usize snformat(char *buf, usize cap, const char *fmt, ...);
