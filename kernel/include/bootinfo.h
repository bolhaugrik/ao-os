/* A stage2 altal a 0x1000 fizikai cimen osszeallitott bootinfo.
 * Az eltolasok a boot/stage2.asm BI_* konstansaival azonosak. */
#pragma once
#include "types.h"

#define BOOTINFO_MAGIC 0x49424F41u /* 'AOBI' little-endian */
#define BOOTINFO_PADDR 0x1000u

struct e820_entry {
    u64 base;
    u64 len;
    u32 type;       /* 1 usable, 2 reserved, 3 ACPI data, 4 ACPI NVS, 5 bad */
    u32 attr;
} PACKED;

#define E820_USABLE   1
#define E820_RESERVED 2
#define E820_ACPI     3
#define E820_NVS      4

struct bootinfo {
    u32 magic;          /* 0  */
    u32 size;           /* 4  */
    u64 boot_tsc;       /* 8  */
    u32 e820_count;     /* 16 */
    u32 e820_paddr;     /* 20 */
    u32 fb_paddr;       /* 24 */
    u32 fb_width;       /* 28 */
    u32 fb_height;      /* 32 */
    u32 fb_pitch;       /* 36 */
    u8  fb_bpp;         /* 40 */
    u8  fb_rpos;        /* 41 */
    u8  fb_gpos;        /* 42 */
    u8  fb_bpos;        /* 43 */
    u16 vbe_mode;       /* 44 */
    u16 pad0;           /* 46 */
    u32 kernel_paddr;   /* 48 */
    u32 kernel_size;    /* 52 */
    u32 ramdisk_paddr;  /* 56 */
    u32 ramdisk_size;   /* 60 */
    u8  boot_drive;     /* 64 */
    u8  pad1[7];        /* 65 */
} PACKED;               /* 72 */

_Static_assert(sizeof(struct bootinfo) == 72, "bootinfo layout");
_Static_assert(sizeof(struct e820_entry) == 24, "e820 layout");
