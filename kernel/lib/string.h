#pragma once
#include "types.h"

void *memset(void *dst, int c, usize n);
void *memcpy(void *dst, const void *src, usize n);
void *memmove(void *dst, const void *src, usize n);
int   memcmp(const void *a, const void *b, usize n);
usize strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, usize n);
char *strcpy(char *dst, const char *src);
usize strlcpy(char *dst, const char *src, usize cap);
bool  str_starts(const char *s, const char *prefix);
