/* AOFS v2: irhato, 4 KiB blokkos, bitmap + inode-tabla, konyvtarak. Nincs symlink,
 * hardlink, jogosultsagi bit. Irasi sorrend: adat -> inode -> bitmap; a superblock
 * dirty flagje jelzi a nem tiszta leallast (boot-kor bitmap-ujraszamolas).
 *
 *   blokk 0        superblock
 *   1..            bitmap (1 bit / blokk)
 *   inode_start..  inode-tabla (128 B / inode, 32 / blokk), 1 = gyoker
 *   adat
 * inode: 12 direkt + 1 indirekt (1024) blokk -> max ~4 MiB / fajl.
 * konyvtar-bejegyzes 64 B: inode, tipus, nevhossz, nev[56]. */
#pragma once
#include "vfs.h"

#define AOFS2_MAGIC 0x32464F41u   /* 'AOF2' */
#define A2_BLOCK 4096
#define A2_SPB   (A2_BLOCK / 512)

extern const struct fs_ops aofs2_ops;

int  aofs2_mkfs(u64 part_lba, u64 part_sectors, const char *label);
/* csatolas; visszaadja az fs-objektumot vagy NULL */
void *aofs2_mount(u64 part_lba, u64 part_sectors, bool *was_dirty);
void aofs2_info(void *fs, u32 *blocks, u32 *free_blocks, u32 *inodes, char *label, usize cap);
