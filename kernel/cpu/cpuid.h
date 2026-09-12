#pragma once
#include "types.h"

struct cpu_info {
    char vendor[13];
    char brand[49];
    u32  family, model, stepping;
    u32  cores;
    bool sse, sse2, sse3, ssse3, sse41, sse42;
    bool nx, lm, invariant_tsc, apic, x2apic;
    bool syscall_ok;
};

void cpuid_read(struct cpu_info *ci);
