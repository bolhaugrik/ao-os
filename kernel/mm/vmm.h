#pragma once
#include "types.h"

/* Phase 1: a stage2 laptablait hasznaljuk (identity + direkt map + higher half,
 * 2 MiB-es lapok). Itt csak a PAT es a framebuffer write-combining leképezese van. */
void vmm_init(void);
void vmm_set_wc(u64 paddr, u64 size);   /* 2 MiB-es lapokra igazitva WC-re allit */
u64  vmm_read_cr3(void);
