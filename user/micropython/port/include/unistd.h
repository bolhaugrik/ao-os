/* AO-OS: csak a tipusok, amiket a MicroPython forrasa var innen */
#pragma once
#include <stddef.h>
typedef long ssize_t;
typedef long off_t;
#define SSIZE_MAX __LONG_MAX__
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
