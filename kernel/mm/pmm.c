/* Fizikai frame-allokator: bitmap, 1 bit / 4 KiB. Kizarolag az E820 "usable"
 * tartomanyai szabadok; az elso 1 MiB, a kernel, a ramdisk es a laptablak foglaltak.
 * Nincs lazy allokacio, nincs overcommit: amit kiadunk, az fizikailag ott van. */
#include "pmm.h"
#include "layout.h"
#include "../lib/string.h"
#include "../cpu/panic.h"

#define MAX_FRAMES (4ULL * 1024 * 1024 * 1024 / PAGE_SIZE)   /* 4 GiB */
static u64 bitmap[MAX_FRAMES / 64];    /* 128 KiB, 1 = foglalt */
static u64 total, free_count, max_paddr, hint;

extern u8 __kernel_end[];

static inline void set_used(u64 f)  { bitmap[f >> 6] |=  (1ULL << (f & 63)); }
static inline void set_free(u64 f)  { bitmap[f >> 6] &= ~(1ULL << (f & 63)); }
static inline bool is_used(u64 f)   { return bitmap[f >> 6] & (1ULL << (f & 63)); }

static void mark_range(u64 start, u64 end, bool used)
{
    if (end > max_paddr)
        end = max_paddr;
    u64 f0 = used ? start / PAGE_SIZE : (start + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 f1 = used ? (end + PAGE_SIZE - 1) / PAGE_SIZE : end / PAGE_SIZE;
    for (u64 f = f0; f < f1; f++) {
        if (used && !is_used(f)) { set_used(f); free_count--; }
        else if (!used && is_used(f)) { set_free(f); free_count++; }
    }
}

void pmm_init(const struct bootinfo *bi)
{
    const struct e820_entry *e = P2V(bi->e820_paddr);
    memset(bitmap, 0xFF, sizeof bitmap);
    free_count = 0;
    max_paddr = 0;
    for (u32 i = 0; i < bi->e820_count; i++)
        if (e[i].type == E820_USABLE && e[i].base + e[i].len > max_paddr)
            max_paddr = e[i].base + e[i].len;
    if (max_paddr > MAX_FRAMES * PAGE_SIZE)
        max_paddr = MAX_FRAMES * PAGE_SIZE;
    total = max_paddr / PAGE_SIZE;

    for (u32 i = 0; i < bi->e820_count; i++)
        if (e[i].type == E820_USABLE)
            mark_range(e[i].base, e[i].base + e[i].len, false);

    /* foglalt: elso 1 MiB (bootinfo, laptablak, BIOS), kernel, ramdisk */
    mark_range(0, 1 * MiB, true);
    mark_range(bi->kernel_paddr, (u64)(uptr)__kernel_end - KERNEL_VMA_BASE, true);
    if (bi->ramdisk_size)
        mark_range(bi->ramdisk_paddr, bi->ramdisk_paddr + bi->ramdisk_size, true);
    hint = 1 * MiB / PAGE_SIZE;
}

u64 pmm_alloc(void)
{
    for (u64 f = hint; f < total; f++) {
        if (!is_used(f)) {
            set_used(f);
            free_count--;
            hint = f + 1;
            return f * PAGE_SIZE;
        }
    }
    for (u64 f = 256; f < hint; f++) {
        if (!is_used(f)) {
            set_used(f);
            free_count--;
            hint = f + 1;
            return f * PAGE_SIZE;
        }
    }
    return 0;
}

u64 pmm_alloc_contig(usize n)
{
    if (n == 0)
        return 0;
    u64 run = 0;
    for (u64 f = 256; f < total; f++) {
        run = is_used(f) ? 0 : run + 1;
        if (run == n) {
            u64 first = f + 1 - n;
            for (u64 k = first; k <= f; k++)
                set_used(k);
            free_count -= n;
            return first * PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free(u64 paddr)
{
    u64 f = paddr / PAGE_SIZE;
    if (f >= total || !is_used(f))
        panic("pmm_free: rossz frame 0x%lx", paddr);
    set_free(f);
    free_count++;
    if (f < hint)
        hint = f;
}

void pmm_free_contig(u64 paddr, usize n)
{
    for (usize i = 0; i < n; i++)
        pmm_free(paddr + i * PAGE_SIZE);
}

u64 pmm_total_frames(void) { return total; }
u64 pmm_free_frames(void) { return free_count; }
u64 pmm_max_paddr(void) { return max_paddr; }
