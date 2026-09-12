#pragma once
#include "types.h"

void  kheap_init(void);
void *kmalloc(usize n);
void *kzalloc(usize n);
void  kfree(void *p);
u64   kheap_used(void);
u64   kheap_reserved(void);
