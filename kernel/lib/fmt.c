#include "fmt.h"
#include "string.h"

static void emit_num(fmt_out_fn out, void *ctx, u64 v, unsigned base, bool neg,
                     int width, bool zero, bool upper)
{
    char tmp[32];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0)
        tmp[n++] = '0';
    while (v) {
        tmp[n++] = digits[v % base];
        v /= base;
    }
    int len = n + (neg ? 1 : 0);
    if (neg && zero)
        out('-', ctx);
    for (int i = len; i < width; i++)
        out(zero ? '0' : ' ', ctx);
    if (neg && !zero)
        out('-', ctx);
    while (n)
        out(tmp[--n], ctx);
}

void vformat(fmt_out_fn out, void *ctx, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out(*fmt, ctx);
            continue;
        }
        fmt++;
        bool zero = false;
        int width = 0;
        int longs = 0;
        if (*fmt == '0') {
            zero = true;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') {
            longs++;
            fmt++;
        }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = (int)strlen(s);
            for (int i = len; i < width; i++)
                out(' ', ctx);
            while (*s)
                out(*s++, ctx);
            break;
        }
        case 'c':
            out((char)va_arg(ap, int), ctx);
            break;
        case 'd': {
            i64 v = longs ? va_arg(ap, i64) : (i64)va_arg(ap, int);
            bool neg = v < 0;
            emit_num(out, ctx, neg ? (u64)(-v) : (u64)v, 10, neg, width, zero, false);
            break;
        }
        case 'u': {
            u64 v = longs ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned);
            emit_num(out, ctx, v, 10, false, width, zero, false);
            break;
        }
        case 'x':
        case 'X': {
            u64 v = longs ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned);
            emit_num(out, ctx, v, 16, false, width, zero, *fmt == 'X');
            break;
        }
        case 'p': {
            u64 v = (u64)(uptr)va_arg(ap, void *);
            out('0', ctx);
            out('x', ctx);
            emit_num(out, ctx, v, 16, false, 16, true, false);
            break;
        }
        case '%':
            out('%', ctx);
            break;
        case 0:
            return;
        default:
            out('%', ctx);
            out(*fmt, ctx);
            break;
        }
    }
}

struct snctx {
    char *buf;
    usize cap;
    usize n;
};

static void sn_out(char c, void *ctx)
{
    struct snctx *s = ctx;
    if (s->n + 1 < s->cap)
        s->buf[s->n] = c;
    s->n++;
}

usize snformat(char *buf, usize cap, const char *fmt, ...)
{
    struct snctx s = { buf, cap, 0 };
    va_list ap;
    va_start(ap, fmt);
    vformat(sn_out, &s, fmt, ap);
    va_end(ap);
    if (cap)
        buf[s.n < cap ? s.n : cap - 1] = 0;
    return s.n;
}
