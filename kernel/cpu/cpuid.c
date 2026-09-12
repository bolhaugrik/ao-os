#include "cpuid.h"
#include "../lib/string.h"

static void cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

void cpuid_read(struct cpu_info *ci)
{
    u32 a, b, c, d;
    memset(ci, 0, sizeof *ci);

    cpuid(0, 0, &a, &b, &c, &d);
    u32 max_leaf = a;
    memcpy(ci->vendor + 0, &b, 4);
    memcpy(ci->vendor + 4, &d, 4);
    memcpy(ci->vendor + 8, &c, 4);
    ci->vendor[12] = 0;

    if (max_leaf >= 1) {
        cpuid(1, 0, &a, &b, &c, &d);
        u32 fam = (a >> 8) & 0xF, model = (a >> 4) & 0xF;
        ci->stepping = a & 0xF;
        if (fam == 0xF) {
            fam += (a >> 20) & 0xFF;
            model |= ((a >> 16) & 0xF) << 4;
        } else if (fam == 6) {
            model |= ((a >> 16) & 0xF) << 4;
        }
        ci->family = fam;
        ci->model = model;
        ci->sse = d & (1 << 25);
        ci->sse2 = d & (1 << 26);
        ci->apic = d & (1 << 9);
        ci->sse3 = c & 1;
        ci->ssse3 = c & (1 << 9);
        ci->sse41 = c & (1 << 19);
        ci->sse42 = c & (1 << 20);
        ci->x2apic = c & (1 << 21);
    }

    cpuid(0x80000000, 0, &a, &b, &c, &d);
    u32 max_ext = a;
    if (max_ext >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        ci->nx = d & (1 << 20);
        ci->lm = d & (1 << 29);
        ci->syscall_ok = d & (1 << 11);
    }
    if (max_ext >= 0x80000004) {
        u32 *p = (u32 *)ci->brand;
        for (u32 i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, 0, &a, &b, &c, &d);
            p[i * 4 + 0] = a;
            p[i * 4 + 1] = b;
            p[i * 4 + 2] = c;
            p[i * 4 + 3] = d;
        }
        ci->brand[48] = 0;
    }
    if (max_ext >= 0x80000007) {
        cpuid(0x80000007, 0, &a, &b, &c, &d);
        ci->invariant_tsc = d & (1 << 8);
    }
    ci->cores = 1;
    if (max_ext >= 0x80000008) {
        cpuid(0x80000008, 0, &a, &b, &c, &d);
        ci->cores = (c & 0xFF) + 1;
    }
}
