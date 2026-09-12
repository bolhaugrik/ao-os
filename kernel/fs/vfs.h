/* Nevter-reteg: kanonikus utvonalak, mount-tabla (max 4), egyseges fajl-muveletek.
 * Nincs symlink, nincs hardlink: a kanonikus utvonal egyertelmu, ezen fut a cap_check. */
#pragma once
#include "types.h"
#include "syscall.h"

#define VFS_PATH_MAX 128
#define VFS_MOUNT_MAX 4

struct handle;
struct mount;

struct fs_ops {
    const char *name;
    int   (*open)(struct mount *m, const char *rel, u32 flags, void **node);
    isize (*read)(struct mount *m, void *node, u64 pos, void *buf, usize n);
    isize (*write)(struct mount *m, void *node, u64 pos, const void *buf, usize n);
    void  (*close)(struct mount *m, void *node);
    int   (*list)(struct mount *m, const char *rel, struct dirent *out, u32 max);
    int   (*stat)(struct mount *m, const char *rel, struct stat *st);
    int   (*mkdir)(struct mount *m, const char *rel);
    int   (*unlink)(struct mount *m, const char *rel);
    int   (*sync)(struct mount *m);
};

struct mount {
    char prefix[32];            /* "/" vagy "/tmp" */
    const struct fs_ops *ops;
    void *fs;
    bool ro;
};

struct vfile {
    struct mount *m;
    void *node;
    u32 flags;
};

void vfs_init(void);
int  vfs_mount(const char *prefix, const struct fs_ops *ops, void *fs, bool ro);
int  vfs_unmount(const char *prefix);
const struct mount *vfs_mount_at(u32 i);

/* cwd + in -> kanonikus abszolut utvonal ("." ".." feloldva, dupla '/' nelkul) */
bool vfs_canon(const char *cwd, const char *in, char *out, usize cap);

int   vfs_open(const char *path, u32 flags, struct handle *h);
isize vfs_read(struct handle *h, void *buf, usize n);
isize vfs_write(struct handle *h, const void *buf, usize n);
void  vfs_close(struct handle *h);
int   vfs_list(const char *path, struct dirent *out, u32 max);
int   vfs_stat(const char *path, struct stat *st);
int   vfs_mkdir(const char *path);
int   vfs_unlink(const char *path);
int   vfs_sync(void);

/* teljes fajl beolvasasa kmalloc-olt pufferbe (kfree a hivo dolga) */
int   vfs_read_all(const char *path, void **buf, usize *size);
/* fajl irasa (letrehozas + csonkolas) */
int   vfs_write_all(const char *path, const void *buf, usize size);
