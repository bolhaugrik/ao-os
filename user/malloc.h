/* Felhasznaloi memoriafoglalo: first-fit szabadlista, osszevonassal, a heap ao_sbrk-kal no
 * (64 KiB-os lepesekben, a manifest mem-korlatjan belul). Szabvanyos nevek, hogy a portolt
 * programok (pl. Doom) valtoztatas nelkul hasznalhassak. */
#pragma once
#include "../kernel/include/types.h"

void *malloc(usize n);
void *calloc(usize count, usize size);
void *realloc(void *p, usize n);
void  free(void *p);
usize malloc_used(void);        /* foglalt bajtok (fejlecekkel) */
usize malloc_heap(void);        /* a heap merete */
