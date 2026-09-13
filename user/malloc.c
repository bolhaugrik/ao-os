#include "malloc.h"
#include "aolib.h"

#define ALIGN 16
#define GROW (64 * 1024)
#define MAGIC_USED 0xA110C8EDu
#define MAGIC_FREE 0xF5EEB10Cu

struct blk {
    usize size;             /* a fejlec nelkuli meret, ALIGN tobbszorose */
    u32 magic;
    u32 pad;
    struct blk *next;       /* csak a szabadlistan */
};
#define HDR ((usize)sizeof(struct blk))

static struct blk *free_list;   /* cim szerint rendezve */
static u8 *heap_lo, *heap_hi;
static usize used_bytes;

static usize round_up(usize n) { return (n + ALIGN - 1) & ~(usize)(ALIGN - 1); }

static void insert_free(struct blk *b)
{
    b->magic = MAGIC_FREE;
    struct blk **pp = &free_list;
    while (*pp && *pp < b) pp = &(*pp)->next;
    b->next = *pp;
    *pp = b;
    /* osszevonas a kovetkezovel, majd az elozovel */
    if (b->next && (u8 *)b + HDR + b->size == (u8 *)b->next) {
        b->size += HDR + b->next->size;
        b->next = b->next->next;
    }
    if (pp != &free_list) {
        struct blk *prev = (struct blk *)((u8 *)pp - __builtin_offsetof(struct blk, next));
        if ((u8 *)prev + HDR + prev->size == (u8 *)b) {
            prev->size += HDR + b->size;
            prev->next = b->next;
        }
    }
}

static bool grow(usize need)
{
    usize n = round_up(need + HDR);
    if (n < GROW) n = GROW;
    n = (n + 4095) & ~(usize)4095;
    if (!heap_lo) { heap_lo = ao_sbrk(0); heap_hi = heap_lo; if (!heap_lo) return false; }
    u8 *old = ao_sbrk((isize)n);
    if (!old) return false;
    heap_hi = old + n;
    struct blk *b = (struct blk *)old;
    b->size = n - HDR;
    insert_free(b);
    return true;
}

void *malloc(usize n)
{
    if (n == 0) n = 1;
    n = round_up(n);
    for (int pass = 0; pass < 2; pass++) {
        struct blk **pp = &free_list;
        while (*pp) {
            struct blk *b = *pp;
            if (b->size >= n) {
                if (b->size >= n + HDR + ALIGN) {          /* szetvagas */
                    struct blk *rest = (struct blk *)((u8 *)b + HDR + n);
                    rest->size = b->size - n - HDR;
                    rest->magic = MAGIC_FREE;
                    rest->next = b->next;
                    *pp = rest;
                    b->size = n;
                } else {
                    *pp = b->next;
                }
                b->magic = MAGIC_USED;
                b->next = NULL;
                used_bytes += HDR + b->size;
                return (u8 *)b + HDR;
            }
            pp = &b->next;
        }
        if (!grow(n)) return NULL;
    }
    return NULL;
}

void free(void *p)
{
    if (!p) return;
    struct blk *b = (struct blk *)((u8 *)p - HDR);
    if (b->magic != MAGIC_USED) { ao_puts("free: rossz mutato vagy dupla free\n"); return; }
    used_bytes -= HDR + b->size;
    insert_free(b);
}

void *calloc(usize count, usize size)
{
    usize n = count * size;
    if (size && n / size != count) return NULL;
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void *realloc(void *p, usize n)
{
    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }
    struct blk *b = (struct blk *)((u8 *)p - HDR);
    if (b->magic != MAGIC_USED) return NULL;
    if (b->size >= round_up(n)) return p;
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, b->size);
    free(p);
    return q;
}

usize malloc_used(void) { return used_bytes; }
usize malloc_heap(void) { return (usize)(heap_hi - heap_lo); }
