/* AO-OS: a printf-csalad a MicroPython shared/libc/printf.c-bol jon (mp_printf-re epul) */
#pragma once
#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int snprintf(char *str, size_t size, const char *fmt, ...);
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);
