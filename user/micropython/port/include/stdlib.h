/* AO-OS: a memoriafoglalo a user/malloc.c, a tobbi a port/libc_ao.c-ben */
#pragma once
#include <stddef.h>

void *malloc(size_t n);
void *calloc(size_t count, size_t size);
void *realloc(void *p, size_t n);
void  free(void *p);
void  abort(void) __attribute__((noreturn));
void  exit(int code) __attribute__((noreturn));

static inline int abs(int x) { return x < 0 ? -x : x; }
static inline long labs(long x) { return x < 0 ? -x : x; }
