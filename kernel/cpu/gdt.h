#pragma once
#include "types.h"

#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x18
#define SEL_UDATA 0x20
#define SEL_TSS   0x28

void gdt_init(void);
void gdt_set_kernel_stack(u64 rsp0);
