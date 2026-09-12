/* Memoria-fajlrendszer: /tmp es /sys. Fa-szerkezet, kmalloc-olt adat. */
#pragma once
#include "vfs.h"

extern const struct fs_ops ramfs_ops;
void *ramfs_create(void);
/* szintetikus fajl: a tartalmat egy callback adja (pl. /sys/audit) */
typedef usize (*ramfs_gen_fn)(char *buf, usize cap);
int ramfs_add_generated(void *fs, const char *path, ramfs_gen_fn fn);
