/* Kernel heap: meretosztalyos free-listak (16 B .. 2 KiB) 4 KiB-es frame-ekbol,
 * a direkt leképezesen at. Nagyobb keres: egybefuggo frame-ek. Nincs slab-keret. */
#include "kheap.h"
#include "pmm.h"
#include "layout.h"
#include "../lib/string.h"
#include "../cpu/panic.h"

#define NCLASS 8
static const u32 class_size[NCLASS] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };

struct hdr {
    u32 magic;      /* 0x4B48 'KH' */
    u32 cls;        /* meretosztaly, vagy 0xFF = nagy blokk */
    u64 frames;     /* nagy blokknal frame-szam */
};
#define HDR_MAGIC 0x00004B48u
#define HDR 16

struct freenode { struct freenode *next; };

static struct freenode *freelist[NCLASS];
static u64 used_bytes, reserved_bytes;

void kheap_init(void)
{
    for (int i = 0; i < NCLASS; i++)
        freelist[i] = NULL;
}

static void refill(int cls)
{
    u64 pa = pmm_alloc();
    if (!pa)
        panic("kheap: nincs szabad frame");
    reserved_bytes += PAGE_SIZE;
    u8 *base = P2V(pa);
    u32 sz = class_size[cls] + HDR;
    for (u32 off = 0; off + sz <= PAGE_SIZE; off += sz) {
        struct freenode *n = (struct freenode *)(base + off);
        n->next = freelist[cls];
        freelist[cls] = n;
    }
}

void *kmalloc(usize n)
{
    if (n == 0)
        n = 1;
    for (int cls = 0; cls < NCLASS; cls++) {
        if (n <= class_size[cls]) {
            if (!freelist[cls])
                refill(cls);
            struct freenode *node = freelist[cls];
            freelist[cls] = node->next;
            struct hdr *h = (struct hdr *)node;
            h->magic = HDR_MAGIC;
            h->cls = (u32)cls;
            h->frames = 0;
            used_bytes += class_size[cls];
            return (u8 *)h + HDR;
        }
    }
    usize frames = (n + HDR + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 pa = pmm_alloc_contig(frames);
    if (!pa)
        panic("kheap: nincs %lu egybefuggo frame", (u64)frames);
    reserved_bytes += frames * PAGE_SIZE;
    struct hdr *h = P2V(pa);
    h->magic = HDR_MAGIC;
    h->cls = 0xFF;
    h->frames = frames;
    used_bytes += frames * PAGE_SIZE;
    return (u8 *)h + HDR;
}

void *kzalloc(usize n)
{
    void *p = kmalloc(n);
    memset(p, 0, n);
    return p;
}

void kfree(void *p)
{
    if (!p)
        return;
    struct hdr *h = (struct hdr *)((u8 *)p - HDR);
    if (h->magic != HDR_MAGIC)
        panic("kfree: rossz blokk %p", p);
    if (h->cls == 0xFF) {
        used_bytes -= h->frames * PAGE_SIZE;
        reserved_bytes -= h->frames * PAGE_SIZE;
        pmm_free_contig(V2P(h), h->frames);
        return;
    }
    u32 cls = h->cls;           /* a freenode felulirja a fejlecet, ezert elobb kiolvassuk */
    used_bytes -= class_size[cls];
    h->magic = 0;
    struct freenode *n = (struct freenode *)h;
    n->next = freelist[cls];
    freelist[cls] = n;
}

u64 kheap_used(void) { return used_bytes; }
u64 kheap_reserved(void) { return reserved_bytes; }
