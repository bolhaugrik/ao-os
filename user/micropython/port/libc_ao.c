/* A MicroPythonnak kello nehany libc-fuggveny, ami a user-libben nincs (a memcpy/strlen/... a
 * kernel/lib/string.c-bol jon, a malloc a user/malloc.c-bol, a printf a shared/libc/printf.c-bol). */
#include "aolib.h"
#include <string.h>
#include <math.h>

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

char *strrchr(const char *s, int c)
{
    const char *r = 0;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}

char *strstr(const char *h, const char *n)
{
    usize nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++)
        if (*h == *n && strncmp(h, n, nl) == 0) return (char *)h;
    return 0;
}

char *strncpy(char *dst, const char *src, usize n)
{
    usize i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

void *memchr(const void *s, int c, usize n)
{
    const u8 *p = s;
    for (usize i = 0; i < n; i++)
        if (p[i] == (u8)c) return (void *)(p + i);
    return 0;
}

double fabs(double x) { return __builtin_fabs(x); }
double nan(const char *tag) { (void)tag; return __builtin_nan(""); }
double log2(double x) { return log(x) / M_LN2; }
double copysign(double x, double y) { return __builtin_copysign(x, y); }   /* a libm_dbl-e csak NDEBUG nelkul fordul */

/* a random modul kezdo magja: az idobelyeg-szamlalo */
unsigned long ao_random_seed(void)
{
    unsigned lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long)hi << 32) | lo;
}

NORETURN void abort(void)
{
    ao_puts("python: abort\n");
    ao_exit(134);
}

NORETURN void exit(int code) { ao_exit(code); }
