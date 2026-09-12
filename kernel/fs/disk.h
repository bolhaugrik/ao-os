/* Lemez-elrendezes es telepites: MBR, AO-particio, panic-tarolo szektor. */
#pragma once
#include "types.h"

#define PANIC_LBA 2047          /* a foglalt terulet utolso szektora, a particio elott */
#define PART_LBA  2048

bool disk_find_partition(u64 *start, u64 *sectors);   /* 0x7F tipusu AO-particio az MBR-ben */
int  disk_mount_root(void);                            /* AOFS2 csatolasa /-re, ramdisk /rd-re */
int  disk_install(const char *label);                  /* teljes telepites a belso lemezre */
int  disk_update(void);                                /* boot-terulet + programok frissitese, /state marad */
int  disk_install_ex(const char *label, bool keep_fs);
int  disk_mkfs(const char *label);                     /* csak a particio formazasa */

void panic_store_write(const char *text);              /* panic-bol hivhato (polling I/O) */
int  panic_store_read(char *buf, usize cap);           /* 0 = nincs, >0 hossz */
void panic_store_clear(void);
