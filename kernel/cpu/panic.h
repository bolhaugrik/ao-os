#pragma once
#include "types.h"

struct regs;

NORETURN void panic(const char *fmt, ...);
NORETURN void panic_exception(const char *name, struct regs *r);
NORETURN void panic_user_exception(const char *name, struct regs *r);   /* csak a task all le */
