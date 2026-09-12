#include "vmm.h"
#include "layout.h"
#include "../arch/io.h"

#define MSR_PAT 0x277
#define PDE_PWT (1ULL << 3)
#define PDE_PS  (1ULL << 7)

u64 vmm_read_cr3(void)
{
    u64 v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static void flush_all(void)
{
    __asm__ volatile("wbinvd; mov %0, %%cr3" : : "r"(vmm_read_cr3()) : "memory");
}

void vmm_init(void)
{
    /* PAT: 0=WB 1=WC 2=UC- 3=UC 4=WB 5=WC 6=UC- 7=UC.
     * A PWT bit (index 1) igy write-combining-ot valaszt. */
    u64 pat = 0x0007010600070106ULL;
    wrmsr(MSR_PAT, pat);
    flush_all();
}

/* A stage2 identity/direkt map PD-i 0x72000-tol, 2048 bejegyzes x 2 MiB. */
void vmm_set_wc(u64 paddr, u64 size)
{
    u64 *pd = P2V(0x72000);
    u64 start = paddr / (2 * MiB);
    u64 end = (paddr + size + 2 * MiB - 1) / (2 * MiB);
    for (u64 i = start; i < end && i < 2048; i++)
        if (pd[i] & PDE_PS)
            pd[i] |= PDE_PWT;
    flush_all();
}
