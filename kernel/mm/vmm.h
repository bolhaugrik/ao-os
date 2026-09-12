#pragma once
#include "types.h"

/* A kernel felso fele (direkt map + higher half) a stage2 laptablaibol jon, es minden
 * cimterben azonos (a PML4 felso 256 bejegyzese megosztott). A user-fel taskonkent kulon. */

#define PTE_P   (1ULL << 0)
#define PTE_W   (1ULL << 1)
#define PTE_U   (1ULL << 2)
#define PTE_PWT (1ULL << 3)
#define PTE_PS  (1ULL << 7)
#define PTE_OWN (1ULL << 9)     /* AVL bit: a frame-et a cimter birtokolja, felszabaditando */
#define PTE_NX  (1ULL << 63)

#define USER_TOP 0x0000800000000000ULL

void vmm_init(void);
void vmm_set_wc(u64 paddr, u64 size);   /* 2 MiB-es lapokra igazitva WC-re allit */
u64  vmm_read_cr3(void);
u64  vmm_boot_pml4(void);

u64  vmm_new_space(void);                                   /* uj PML4, kernel-fel masolva */
bool vmm_map(u64 pml4, u64 vaddr, u64 paddr, u64 flags);    /* 4 KiB lap, tablak allokalasa */
u64  vmm_map_new(u64 pml4, u64 vaddr, u64 flags);           /* uj, nullazott frame; 0 ha nincs */
u64  vmm_lookup(u64 pml4, u64 vaddr);                       /* fizikai cim vagy 0 */
void vmm_destroy_space(u64 pml4);                           /* user-fel + birtokolt frame-ek */
void vmm_switch(u64 pml4);
