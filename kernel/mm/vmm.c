#include "vmm.h"
#include "pmm.h"
#include "layout.h"
#include "../arch/io.h"
#include "../lib/string.h"

#define MSR_PAT 0x277
#define BOOT_PML4 0x70000ULL

u64 vmm_read_cr3(void)
{
    u64 v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

u64 vmm_boot_pml4(void) { return BOOT_PML4; }

void vmm_switch(u64 pml4)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4) : "memory");
}

static void flush_all(void)
{
    __asm__ volatile("wbinvd; mov %0, %%cr3" : : "r"(vmm_read_cr3()) : "memory");
}

void vmm_init(void)
{
    /* PAT: 0=WB 1=WC 2=UC- 3=UC 4=WB 5=WC 6=UC- 7=UC. A PWT bit (index 1) igy WC-t valaszt. */
    wrmsr(MSR_PAT, 0x0007010600070106ULL);
    flush_all();
}

/* A stage2 identity/direkt map PD-i 0x72000-tol, 2048 bejegyzes x 2 MiB. */
void vmm_set_wc(u64 paddr, u64 size)
{
    u64 *pd = P2V(0x72000);
    u64 start = paddr / (2 * MiB);
    u64 end = (paddr + size + 2 * MiB - 1) / (2 * MiB);
    for (u64 i = start; i < end && i < 2048; i++)
        if (pd[i] & PTE_PS)
            pd[i] |= PTE_PWT;
    flush_all();
}

/* ---------------------------------------------------------------- cimterek */
static u64 alloc_table(void)
{
    u64 pa = pmm_alloc();
    if (pa)
        memset(P2V(pa), 0, PAGE_SIZE);
    return pa;
}

u64 vmm_new_space(void)
{
    u64 pa = alloc_table();
    if (!pa)
        return 0;
    u64 *dst = P2V(pa);
    const u64 *src = P2V(BOOT_PML4);
    for (int i = 256; i < 512; i++)
        dst[i] = src[i];
    return pa;
}

/* tabla-bejegyzes elerese/letrehozasa; visszaadja a kovetkezo szint fizikai cimet */
static u64 *next_level(u64 *table, u32 idx, bool create)
{
    if (!(table[idx] & PTE_P)) {
        if (!create)
            return NULL;
        u64 pa = alloc_table();
        if (!pa)
            return NULL;
        table[idx] = pa | PTE_P | PTE_W | PTE_U;
    }
    return P2V(table[idx] & 0x000FFFFFFFFFF000ULL);
}

bool vmm_map(u64 pml4, u64 vaddr, u64 paddr, u64 flags)
{
    if (vaddr >= USER_TOP)
        return false;
    u64 *l4 = P2V(pml4);
    u64 *l3 = next_level(l4, (vaddr >> 39) & 511, true);
    if (!l3) return false;
    u64 *l2 = next_level(l3, (vaddr >> 30) & 511, true);
    if (!l2) return false;
    u64 *l1 = next_level(l2, (vaddr >> 21) & 511, true);
    if (!l1) return false;
    u32 i = (vaddr >> 12) & 511;
    if (l1[i] & PTE_P)
        return false;
    l1[i] = (paddr & 0x000FFFFFFFFFF000ULL) | (flags & 0xFFF) | (flags & PTE_OWN) | PTE_P;
    return true;
}

u64 vmm_map_new(u64 pml4, u64 vaddr, u64 flags)
{
    u64 pa = alloc_table();
    if (!pa)
        return 0;
    if (!vmm_map(pml4, vaddr, pa, flags | PTE_OWN)) {
        pmm_free(pa);
        return 0;
    }
    return pa;
}

u64 vmm_lookup(u64 pml4, u64 vaddr)
{
    u64 *l4 = P2V(pml4);
    u64 *l3 = next_level(l4, (vaddr >> 39) & 511, false);
    if (!l3) return 0;
    u64 *l2 = next_level(l3, (vaddr >> 30) & 511, false);
    if (!l2) return 0;
    u64 *l1 = next_level(l2, (vaddr >> 21) & 511, false);
    if (!l1) return 0;
    u64 e = l1[(vaddr >> 12) & 511];
    return (e & PTE_P) ? (e & 0x000FFFFFFFFFF000ULL) | (vaddr & 0xFFF) : 0;
}

void vmm_destroy_space(u64 pml4)
{
    u64 *l4 = P2V(pml4);
    for (int i4 = 0; i4 < 256; i4++) {
        if (!(l4[i4] & PTE_P)) continue;
        u64 *l3 = P2V(l4[i4] & 0x000FFFFFFFFFF000ULL);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(l3[i3] & PTE_P)) continue;
            u64 *l2 = P2V(l3[i3] & 0x000FFFFFFFFFF000ULL);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(l2[i2] & PTE_P)) continue;
                u64 *l1 = P2V(l2[i2] & 0x000FFFFFFFFFF000ULL);
                for (int i1 = 0; i1 < 512; i1++)
                    if ((l1[i1] & PTE_P) && (l1[i1] & PTE_OWN))
                        pmm_free(l1[i1] & 0x000FFFFFFFFFF000ULL);
                pmm_free(l2[i2] & 0x000FFFFFFFFFF000ULL);
            }
            pmm_free(l3[i3] & 0x000FFFFFFFFFF000ULL);
        }
        pmm_free(l4[i4] & 0x000FFFFFFFFFF000ULL);
    }
    pmm_free(pml4);
}
