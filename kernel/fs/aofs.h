/* AOFS v1: csak olvashato, lapos nevter, host-oldalon epitett image (tools/mkaofs.py).
 *
 *   superblock (4096 B): magic "AOFS", u32 version=1, u32 count, u32 total_size
 *   entry-tabla:         count x 128 B: name[96], u32 type, u32 offset, u32 size, u32 flags, pad[12]
 *   adatterulet
 *
 * A nev a teljes utvonal vezeto '/' nelkul ("bin/hello.aox"), konyvtar type=2. */
#pragma once
#include "types.h"

#define AOFS_MAGIC 0x53464F41u   /* 'AOFS' */
#define AOFS_FILE 1
#define AOFS_DIR  2

struct aofs_entry {
    char name[96];
    u32 type;
    u32 offset;
    u32 size;
    u32 flags;
    u8  pad[12];
} PACKED;

struct aofs_super {
    u32 magic;
    u32 version;
    u32 count;
    u32 total_size;
} PACKED;

bool aofs_mount(const void *image, u32 size);
bool aofs_mounted(void);
u32  aofs_count(void);
const struct aofs_entry *aofs_entry(u32 i);
const struct aofs_entry *aofs_lookup(const char *path);   /* "/a/b" vagy "a/b" */
const void *aofs_data(const struct aofs_entry *e);
u32  aofs_image_size(void);
