#pragma once
#include "types.h"
#include "bootinfo.h"

#define PAGE_SIZE 4096ULL

void pmm_init(const struct bootinfo *bi);
u64  pmm_alloc(void);                 /* egy 4 KiB frame fizikai cime, 0 ha nincs */
u64  pmm_alloc_contig(usize n);       /* n egymas utani frame */
void pmm_free(u64 paddr);
void pmm_free_contig(u64 paddr, usize n);

u64 pmm_total_frames(void);
u64 pmm_free_frames(void);
u64 pmm_max_paddr(void);
