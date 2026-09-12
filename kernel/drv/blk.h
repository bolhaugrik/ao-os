/* Blokk-eszkoz interfesz: az egyetlen SATA-lemez (AHCI). Nincs eszkozfajl, nincs
 * referenciaszamlalas: harom fuggveny es ket lekerdezes. */
#pragma once
#include "types.h"

#define SECTOR_SIZE 512

bool blk_present(void);
u64  blk_sectors(void);
const char *blk_model(void);
int  blk_read(u64 lba, u32 count, void *buf);          /* 0 vagy E_IO */
int  blk_write(u64 lba, u32 count, const void *buf);
int  blk_flush(void);
