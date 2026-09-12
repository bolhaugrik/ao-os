/* Virtualis cimter-elrendezes (lasd docs/AO-OS-Architecture-v0.1.md 4.3). */
#pragma once
#include "types.h"

#define DIRECT_MAP_BASE 0xFFFF800000000000ULL   /* fizikai memoria tukre */
#define KERNEL_VMA_BASE 0xFFFFFFFF80000000ULL   /* kernel -2 GiB */

static inline void *P2V(u64 paddr) { return (void *)(uptr)(paddr + DIRECT_MAP_BASE); }
static inline u64 V2P(const void *vaddr) { return (u64)(uptr)vaddr - DIRECT_MAP_BASE; }
