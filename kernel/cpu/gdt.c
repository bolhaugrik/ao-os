/* GDT: null, kernel kod/adat, user kod/adat (Phase 2), TSS. */
#include "gdt.h"
#include "../lib/string.h"

struct tss {
    u32 res0;
    u64 rsp0, rsp1, rsp2;
    u64 res1;
    u64 ist[7];
    u64 res2;
    u16 res3;
    u16 iomap_base;
} PACKED;

struct gdtr {
    u16 limit;
    u64 base;
} PACKED;

static u64 gdt[7] ALIGNED(16);
static struct tss tss ALIGNED(16);
static u8 df_stack[8192] ALIGNED(16);   /* IST1: double fault */

extern void gdt_load(const struct gdtr *g, u16 tss_sel);

void gdt_init(void)
{
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;   /* kernel kod, L=1 */
    gdt[2] = 0x00CF92000000FFFFULL;   /* kernel adat */
    gdt[3] = 0x00AFFA000000FFFFULL;   /* user kod, DPL3 */
    gdt[4] = 0x00CFF2000000FFFFULL;   /* user adat, DPL3 */

    memset(&tss, 0, sizeof tss);
    tss.iomap_base = sizeof tss;
    tss.ist[0] = (u64)(uptr)(df_stack + sizeof df_stack);

    u64 base = (u64)(uptr)&tss;
    u32 limit = sizeof tss - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
             (((u64)limit & 0xF0000) << 32) | ((base & 0xFF000000) << 32);
    gdt[6] = base >> 32;

    struct gdtr g = { sizeof gdt - 1, (u64)(uptr)gdt };
    gdt_load(&g, SEL_TSS);
}

void gdt_set_kernel_stack(u64 rsp0)
{
    tss.rsp0 = rsp0;
}
