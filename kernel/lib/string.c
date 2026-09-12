/* Sajat memoria- es string-fuggvenyek. A fordito generalt kodbol is hivhatja oket.
 * memset/memcpy rep stos/movs-szal: a framebuffer es a konzol-puffer miatt szamit. */
#include "string.h"

void *memset(void *dst, int c, usize n)
{
    u64 v = (u8)c;
    v |= v << 8; v |= v << 16; v |= v << 32;
    void *d = dst;
    usize q = n >> 3, r = n & 7;
    __asm__ volatile("rep stosq" : "+D"(d), "+c"(q) : "a"(v) : "memory");
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(r) : "a"(v) : "memory");
    return dst;
}

void *memcpy(void *dst, const void *src, usize n)
{
    void *d = dst;
    const void *s = src;
    usize q = n >> 3, r = n & 7;
    __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(q) : : "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(r) : : "memory");
    return dst;
}

void *memmove(void *dst, const void *src, usize n)
{
    u8 *d = dst;
    const u8 *s = src;
    if (d == s || n == 0)
        return dst;
    if (d < s || d >= s + n)
        return memcpy(dst, src, n);
    d += n;
    s += n;
    while (n--)
        *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, usize n)
{
    const u8 *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

usize strlen(const char *s)
{
    usize n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, usize n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a)
            return (u8)*a - (u8)*b;
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

usize strlcpy(char *dst, const char *src, usize cap)
{
    usize n = strlen(src);
    if (cap) {
        usize c = n < cap - 1 ? n : cap - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}

bool str_starts(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++)
            return false;
    return true;
}
