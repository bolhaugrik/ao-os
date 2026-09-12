#pragma once
#include "types.h"

/* A sorrend a SYSRET miatt kotott: STAR[63:48] = 0x10, SS = +8 (udata), CS = +16 (ucode). */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA 0x18
#define SEL_UCODE 0x20
#define SEL_TSS   0x28
#define SEL_USER_CS (SEL_UCODE | 3)
#define SEL_USER_SS (SEL_UDATA | 3)

void gdt_init(void);
void gdt_set_kernel_stack(u64 rsp0);
