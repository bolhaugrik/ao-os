/* AO-OS: a fordito sajat limits.h-ja + a POSIX SSIZE_MAX */
#pragma once
#include_next <limits.h>
#ifndef SSIZE_MAX
#define SSIZE_MAX __LONG_MAX__
#endif
