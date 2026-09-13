#include "aofs2.h"
#include "../drv/blk.h"
#include "../cpu/pit.h"
#include "../mm/kheap.h"
#include "../lib/string.h"

struct a2_super {
    u32 magic, version, block_count, block_size;
    u32 bitmap_start, bitmap_blocks, inode_start, inode_blocks, inode_count, root_inode;
    u32 dirty, free_blocks;
    u64 mount_count;
    char label[32];
} PACKED;

struct a2_inode {
    u32 type;           /* 0 szabad, 1 fajl, 2 konyvtar */
    u32 nlink;
    u64 size;
    u64 mtime;
    u32 direct[12];
    u32 indirect;
    u32 dindirect;      /* ketszeres indirekt: 1024 tabla x 1024 blokk (4 GiB); a regi inode-okban 0 */
    u32 pad[12];
} PACKED;
_Static_assert(sizeof(struct a2_inode) == 128, "inode");

struct a2_dirent {
    u32 inode;
    u8  type;
    u8  name_len;
    u8  pad[2];
    char name[56];
} PACKED;
_Static_assert(sizeof(struct a2_dirent) == 64, "dirent");

#define IPB (A2_BLOCK / 128)        /* inode / blokk */
#define DPB (A2_BLOCK / 64)         /* dirent / blokk */
#define NDIRECT 12
#define NIND (A2_BLOCK / 4)
#define CACHE_N 16

struct cache_ent {
    u32 block;
    bool valid;
    u8 data[A2_BLOCK];
};

struct a2fs {
    u64 part_lba;
    struct a2_super sb;
    u8 *bitmap;                 /* bitmap_blocks * 4096 */
    struct cache_ent *cache;
    u32 cache_next;
    bool sb_dirty_on_disk;
};

/* ---------------------------------------------------------------- blokk I/O + cache */
static int raw_read(struct a2fs *fs, u32 block, void *buf)
{
    return blk_read(fs->part_lba + (u64)block * A2_SPB, A2_SPB, buf);
}

static int raw_write(struct a2fs *fs, u32 block, const void *buf)
{
    return blk_write(fs->part_lba + (u64)block * A2_SPB, A2_SPB, buf);
}

static int bread(struct a2fs *fs, u32 block, void *buf)
{
    for (u32 i = 0; i < CACHE_N; i++)
        if (fs->cache[i].valid && fs->cache[i].block == block) {
            memcpy(buf, fs->cache[i].data, A2_BLOCK);
            return 0;
        }
    int e = raw_read(fs, block, buf);
    if (e) return e;
    struct cache_ent *c = &fs->cache[fs->cache_next];
    fs->cache_next = (fs->cache_next + 1) % CACHE_N;
    c->block = block;
    c->valid = true;
    memcpy(c->data, buf, A2_BLOCK);
    return 0;
}

static int mark_dirty(struct a2fs *fs)
{
    if (fs->sb_dirty_on_disk) return 0;
    fs->sb.dirty = 1;
    u8 blk[A2_BLOCK];
    memset(blk, 0, sizeof blk);
    memcpy(blk, &fs->sb, sizeof fs->sb);
    int e = raw_write(fs, 0, blk);
    if (e) return e;
    fs->sb_dirty_on_disk = true;
    return 0;
}

static int bwrite(struct a2fs *fs, u32 block, const void *buf)
{
    int e = mark_dirty(fs);
    if (e) return e;
    e = raw_write(fs, block, buf);
    if (e) return e;
    for (u32 i = 0; i < CACHE_N; i++)
        if (fs->cache[i].valid && fs->cache[i].block == block) {
            memcpy(fs->cache[i].data, buf, A2_BLOCK);
            return 0;
        }
    struct cache_ent *c = &fs->cache[fs->cache_next];
    fs->cache_next = (fs->cache_next + 1) % CACHE_N;
    c->block = block;
    c->valid = true;
    memcpy(c->data, buf, A2_BLOCK);
    return 0;
}

/* ---------------------------------------------------------------- bitmap */
static inline bool bm_test(struct a2fs *fs, u32 b) { return fs->bitmap[b >> 3] & (1 << (b & 7)); }
static inline void bm_set(struct a2fs *fs, u32 b) { fs->bitmap[b >> 3] |= (u8)(1 << (b & 7)); }
static inline void bm_clr(struct a2fs *fs, u32 b) { fs->bitmap[b >> 3] &= (u8)~(1 << (b & 7)); }

static int bm_flush_block_of(struct a2fs *fs, u32 b)
{
    u32 idx = b / (A2_BLOCK * 8);
    return bwrite(fs, fs->sb.bitmap_start + idx, fs->bitmap + (usize)idx * A2_BLOCK);
}

static u32 balloc(struct a2fs *fs)
{
    u32 first = fs->sb.inode_start + fs->sb.inode_blocks;
    for (u32 b = first; b < fs->sb.block_count; b++) {
        if (!bm_test(fs, b)) {
            bm_set(fs, b);
            fs->sb.free_blocks--;
            u8 zero[A2_BLOCK];
            memset(zero, 0, sizeof zero);
            if (bwrite(fs, b, zero)) { bm_clr(fs, b); fs->sb.free_blocks++; return 0; }
            bm_flush_block_of(fs, b);
            return b;
        }
    }
    return 0;
}

static void bfree(struct a2fs *fs, u32 b)
{
    if (!b || b >= fs->sb.block_count) return;
    bm_clr(fs, b);
    fs->sb.free_blocks++;
    bm_flush_block_of(fs, b);
}

/* ---------------------------------------------------------------- inode-ok */
static int iread(struct a2fs *fs, u32 ino, struct a2_inode *out)
{
    if (ino == 0 || ino >= fs->sb.inode_count) return E_INVAL;
    u8 blk[A2_BLOCK];
    int e = bread(fs, fs->sb.inode_start + ino / IPB, blk);
    if (e) return e;
    memcpy(out, blk + (ino % IPB) * 128, 128);
    return 0;
}

static int iwrite(struct a2fs *fs, u32 ino, const struct a2_inode *in)
{
    u8 blk[A2_BLOCK];
    int e = bread(fs, fs->sb.inode_start + ino / IPB, blk);
    if (e) return e;
    memcpy(blk + (ino % IPB) * 128, in, 128);
    return bwrite(fs, fs->sb.inode_start + ino / IPB, blk);
}

static u32 ialloc(struct a2fs *fs, u32 type)
{
    for (u32 ino = 1; ino < fs->sb.inode_count; ino++) {
        struct a2_inode in;
        if (iread(fs, ino, &in)) return 0;
        if (in.type == 0) {
            memset(&in, 0, sizeof in);
            in.type = type;
            in.nlink = 1;
            in.mtime = pit_ticks();
            if (iwrite(fs, ino, &in)) return 0;
            return ino;
        }
    }
    return 0;
}

/* a fajl idx-edik blokkja; alloc eseten letrehozza */
static u32 imap(struct a2fs *fs, struct a2_inode *in, u32 ino, u32 idx, bool alloc)
{
    if (idx < NDIRECT) {
        if (!in->direct[idx] && alloc) {
            in->direct[idx] = balloc(fs);
            if (in->direct[idx]) iwrite(fs, ino, in);
        }
        return in->direct[idx];
    }
    idx -= NDIRECT;
    u32 tab[NIND];
    if (idx < NIND) {
        if (!in->indirect) {
            if (!alloc) return 0;
            in->indirect = balloc(fs);
            if (!in->indirect) return 0;
            iwrite(fs, ino, in);
        }
        if (bread(fs, in->indirect, tab)) return 0;
        if (!tab[idx] && alloc) {
            tab[idx] = balloc(fs);
            if (tab[idx]) bwrite(fs, in->indirect, tab);
        }
        return tab[idx];
    }
    /* ketszeres indirekt */
    idx -= NIND;
    if (idx >= NIND * NIND) return 0;
    if (!in->dindirect) {
        if (!alloc) return 0;
        in->dindirect = balloc(fs);
        if (!in->dindirect) return 0;
        iwrite(fs, ino, in);
    }
    if (bread(fs, in->dindirect, tab)) return 0;
    u32 t1 = idx / NIND, t2 = idx % NIND;
    if (!tab[t1]) {
        if (!alloc) return 0;
        tab[t1] = balloc(fs);
        if (!tab[t1]) return 0;
        bwrite(fs, in->dindirect, tab);
    }
    u32 sub = tab[t1];
    if (bread(fs, sub, tab)) return 0;
    if (!tab[t2] && alloc) {
        tab[t2] = balloc(fs);
        if (tab[t2]) bwrite(fs, sub, tab);
    }
    return tab[t2];
}

static void ifree_blocks(struct a2fs *fs, struct a2_inode *in)
{
    for (u32 i = 0; i < NDIRECT; i++) { bfree(fs, in->direct[i]); in->direct[i] = 0; }
    u32 tab[NIND];
    if (in->indirect) {
        if (bread(fs, in->indirect, tab) == 0)
            for (u32 i = 0; i < NIND; i++) bfree(fs, tab[i]);
        bfree(fs, in->indirect);
        in->indirect = 0;
    }
    if (in->dindirect) {
        if (bread(fs, in->dindirect, tab) == 0) {
            for (u32 i = 0; i < NIND; i++) {
                if (!tab[i]) continue;
                u32 sub[NIND];
                if (bread(fs, tab[i], sub) == 0)
                    for (u32 k = 0; k < NIND; k++) bfree(fs, sub[k]);
                bfree(fs, tab[i]);
            }
        }
        bfree(fs, in->dindirect);
        in->dindirect = 0;
    }
    in->size = 0;
}

static isize file_read(struct a2fs *fs, u32 ino, u64 pos, void *buf, usize n)
{
    struct a2_inode in;
    if (iread(fs, ino, &in)) return E_IO;
    if (pos >= in.size) return 0;
    if (pos + n > in.size) n = in.size - pos;
    u8 *d = buf;
    usize done = 0;
    u8 blk[A2_BLOCK];
    while (done < n) {
        u32 idx = (u32)((pos + done) / A2_BLOCK);
        u32 off = (u32)((pos + done) % A2_BLOCK);
        u32 c = A2_BLOCK - off;
        if (c > n - done) c = (u32)(n - done);
        u32 b = imap(fs, &in, ino, idx, false);
        if (!b) memset(d + done, 0, c);
        else {
            if (bread(fs, b, blk)) return E_IO;
            memcpy(d + done, blk + off, c);
        }
        done += c;
    }
    return (isize)done;
}

static isize file_write(struct a2fs *fs, u32 ino, u64 pos, const void *buf, usize n)
{
    struct a2_inode in;
    if (iread(fs, ino, &in)) return E_IO;
    const u8 *s = buf;
    usize done = 0;
    u8 blk[A2_BLOCK];
    while (done < n) {
        u32 idx = (u32)((pos + done) / A2_BLOCK);
        u32 off = (u32)((pos + done) % A2_BLOCK);
        u32 c = A2_BLOCK - off;
        if (c > n - done) c = (u32)(n - done);
        u32 b = imap(fs, &in, ino, idx, true);
        if (!b) return done ? (isize)done : E_NOMEM;
        if (off || c < A2_BLOCK) { if (bread(fs, b, blk)) return E_IO; }
        memcpy(blk + off, s + done, c);
        if (bwrite(fs, b, blk)) return E_IO;
        done += c;
    }
    if (pos + done > in.size) in.size = pos + done;
    in.mtime = pit_ticks();
    if (iwrite(fs, ino, &in)) return E_IO;
    return (isize)done;
}

/* ---------------------------------------------------------------- konyvtarak */
static u32 dir_lookup(struct a2fs *fs, u32 dir, const char *name, usize len, u8 *type)
{
    struct a2_inode in;
    if (iread(fs, dir, &in) || in.type != 2) return 0;
    u8 blk[A2_BLOCK];
    for (u32 idx = 0; (u64)idx * A2_BLOCK < in.size; idx++) {
        u32 b = imap(fs, &in, dir, idx, false);
        if (!b || bread(fs, b, blk)) continue;
        struct a2_dirent *de = (struct a2_dirent *)blk;
        for (u32 i = 0; i < DPB; i++)
            if (de[i].inode && de[i].name_len == len && memcmp(de[i].name, name, len) == 0) {
                if (type) *type = de[i].type;
                return de[i].inode;
            }
    }
    return 0;
}

static int dir_add(struct a2fs *fs, u32 dir, const char *name, usize len, u32 ino, u8 type)
{
    if (len == 0 || len > 56) return E_INVAL;
    struct a2_inode in;
    if (iread(fs, dir, &in) || in.type != 2) return E_NOTDIR;
    u8 blk[A2_BLOCK];
    u32 nblocks = (u32)((in.size + A2_BLOCK - 1) / A2_BLOCK);
    for (u32 idx = 0; idx <= nblocks; idx++) {
        u32 b = imap(fs, &in, dir, idx, true);
        if (!b) return E_NOMEM;
        if (bread(fs, b, blk)) return E_IO;
        struct a2_dirent *de = (struct a2_dirent *)blk;
        for (u32 i = 0; i < DPB; i++) {
            if (de[i].inode) continue;
            memset(&de[i], 0, sizeof de[i]);
            de[i].inode = ino;
            de[i].type = type;
            de[i].name_len = (u8)len;
            memcpy(de[i].name, name, len);
            if (bwrite(fs, b, blk)) return E_IO;
            u64 end = (u64)idx * A2_BLOCK + (u64)(i + 1) * 64;
            if (end > in.size) { in.size = end; in.mtime = pit_ticks(); iwrite(fs, dir, &in); }
            return 0;
        }
    }
    return E_NOMEM;
}

static int dir_remove(struct a2fs *fs, u32 dir, const char *name, usize len)
{
    struct a2_inode in;
    if (iread(fs, dir, &in) || in.type != 2) return E_NOTDIR;
    u8 blk[A2_BLOCK];
    for (u32 idx = 0; (u64)idx * A2_BLOCK < in.size; idx++) {
        u32 b = imap(fs, &in, dir, idx, false);
        if (!b || bread(fs, b, blk)) continue;
        struct a2_dirent *de = (struct a2_dirent *)blk;
        for (u32 i = 0; i < DPB; i++)
            if (de[i].inode && de[i].name_len == len && memcmp(de[i].name, name, len) == 0) {
                memset(&de[i], 0, sizeof de[i]);
                return bwrite(fs, b, blk);
            }
    }
    return E_NOENT;
}

static bool dir_empty(struct a2fs *fs, u32 dir)
{
    struct a2_inode in;
    if (iread(fs, dir, &in)) return false;
    u8 blk[A2_BLOCK];
    for (u32 idx = 0; (u64)idx * A2_BLOCK < in.size; idx++) {
        u32 b = imap(fs, &in, dir, idx, false);
        if (!b || bread(fs, b, blk)) continue;
        struct a2_dirent *de = (struct a2_dirent *)blk;
        for (u32 i = 0; i < DPB; i++) if (de[i].inode) return false;
    }
    return true;
}

/* utvonal feloldasa; parent + utolso komponens opcionalisan */
static u32 walk(struct a2fs *fs, const char *rel, u32 *parent, const char **last, usize *last_len, u8 *type)
{
    u32 cur = fs->sb.root_inode;
    u8 t = 2;
    const char *p = rel;
    if (parent) { *parent = 0; *last = ""; *last_len = 0; }
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        usize len = (usize)(p - start);
        while (*p == '/') p++;
        bool last_comp = (*p == 0);
        if (last_comp && parent) { *parent = cur; *last = start; *last_len = len; }
        u32 next = dir_lookup(fs, cur, start, len, &t);
        if (!next) return 0;
        if (!last_comp && t != 2) return 0;
        cur = next;
    }
    if (type) *type = t;
    return cur;
}

/* ---------------------------------------------------------------- mkfs / mount */
int aofs2_mkfs(u64 part_lba, u64 part_sectors, const char *label)
{
    if (!blk_present()) return E_IO;
    u32 blocks = (u32)(part_sectors / A2_SPB);
    if (blocks < 64) return E_INVAL;
    struct a2_super sb;
    memset(&sb, 0, sizeof sb);
    sb.magic = AOFS2_MAGIC;
    sb.version = 2;
    sb.block_count = blocks;
    sb.block_size = A2_BLOCK;
    sb.bitmap_start = 1;
    sb.bitmap_blocks = (blocks + A2_BLOCK * 8 - 1) / (A2_BLOCK * 8);
    sb.inode_count = blocks / 8;
    if (sb.inode_count < 256) sb.inode_count = 256;
    if (sb.inode_count > 32768) sb.inode_count = 32768;
    sb.inode_blocks = (sb.inode_count + IPB - 1) / IPB;
    sb.inode_start = sb.bitmap_start + sb.bitmap_blocks;
    sb.root_inode = 1;
    sb.dirty = 0;
    sb.mount_count = 0;
    strlcpy(sb.label, label ? label : "ao", sizeof sb.label);
    u32 meta = sb.inode_start + sb.inode_blocks;
    sb.free_blocks = blocks - meta;

    u8 *blk = kmalloc(A2_BLOCK);
    /* bitmap: meta-blokkok foglaltak */
    for (u32 i = 0; i < sb.bitmap_blocks; i++) {
        memset(blk, 0, A2_BLOCK);
        for (u32 b = i * A2_BLOCK * 8; b < (i + 1) * A2_BLOCK * 8 && b < blocks; b++)
            if (b < meta) blk[(b - i * A2_BLOCK * 8) >> 3] |= (u8)(1 << (b & 7));
        if (blk_write(part_lba + (u64)(sb.bitmap_start + i) * A2_SPB, A2_SPB, blk)) { kfree(blk); return E_IO; }
    }
    /* inode-tabla nullazva, gyoker konyvtar */
    memset(blk, 0, A2_BLOCK);
    for (u32 i = 0; i < sb.inode_blocks; i++) {
        if (i == 0) {
            struct a2_inode *root = (struct a2_inode *)(blk + 128);
            root->type = 2;
            root->nlink = 1;
            root->mtime = pit_ticks();
        }
        if (blk_write(part_lba + (u64)(sb.inode_start + i) * A2_SPB, A2_SPB, blk)) { kfree(blk); return E_IO; }
        memset(blk, 0, A2_BLOCK);
    }
    memset(blk, 0, A2_BLOCK);
    memcpy(blk, &sb, sizeof sb);
    int e = blk_write(part_lba, A2_SPB, blk);
    kfree(blk);
    blk_flush();
    return e;
}

/* piszkos leallas utan: bitmap ujraszamolasa az inode-okbol */
static void fsck_bitmap(struct a2fs *fs)
{
    u32 meta = fs->sb.inode_start + fs->sb.inode_blocks;
    memset(fs->bitmap, 0, (usize)fs->sb.bitmap_blocks * A2_BLOCK);
    for (u32 b = 0; b < meta; b++) bm_set(fs, b);
    u32 used = meta;
    for (u32 ino = 1; ino < fs->sb.inode_count; ino++) {
        struct a2_inode in;
        if (iread(fs, ino, &in) || in.type == 0) continue;
        for (u32 i = 0; i < NDIRECT; i++)
            if (in.direct[i] && in.direct[i] < fs->sb.block_count && !bm_test(fs, in.direct[i])) { bm_set(fs, in.direct[i]); used++; }
        u32 tab[NIND];
        if (in.indirect && in.indirect < fs->sb.block_count) {
            if (!bm_test(fs, in.indirect)) { bm_set(fs, in.indirect); used++; }
            if (bread(fs, in.indirect, tab) == 0)
                for (u32 i = 0; i < NIND; i++)
                    if (tab[i] && tab[i] < fs->sb.block_count && !bm_test(fs, tab[i])) { bm_set(fs, tab[i]); used++; }
        }
        if (in.dindirect && in.dindirect < fs->sb.block_count) {
            if (!bm_test(fs, in.dindirect)) { bm_set(fs, in.dindirect); used++; }
            if (bread(fs, in.dindirect, tab) == 0) {
                for (u32 i = 0; i < NIND; i++) {
                    if (!tab[i] || tab[i] >= fs->sb.block_count) continue;
                    if (!bm_test(fs, tab[i])) { bm_set(fs, tab[i]); used++; }
                    u32 sub[NIND];
                    if (bread(fs, tab[i], sub) == 0)
                        for (u32 k = 0; k < NIND; k++)
                            if (sub[k] && sub[k] < fs->sb.block_count && !bm_test(fs, sub[k])) { bm_set(fs, sub[k]); used++; }
                }
            }
        }
    }
    fs->sb.free_blocks = fs->sb.block_count - used;
    for (u32 i = 0; i < fs->sb.bitmap_blocks; i++)
        raw_write(fs, fs->sb.bitmap_start + i, fs->bitmap + (usize)i * A2_BLOCK);
}

void *aofs2_mount(u64 part_lba, u64 part_sectors, bool *was_dirty)
{
    if (!blk_present()) return NULL;
    u8 *blk = kmalloc(A2_BLOCK);
    if (blk_read(part_lba, A2_SPB, blk)) { kfree(blk); return NULL; }
    struct a2_super *sb = (struct a2_super *)blk;
    if (sb->magic != AOFS2_MAGIC || sb->version != 2 || sb->block_size != A2_BLOCK ||
        (u64)sb->block_count * A2_SPB > part_sectors) { kfree(blk); return NULL; }
    struct a2fs *fs = kzalloc(sizeof *fs);
    fs->part_lba = part_lba;
    fs->sb = *sb;
    kfree(blk);
    fs->bitmap = kmalloc((usize)fs->sb.bitmap_blocks * A2_BLOCK);
    fs->cache = kzalloc(sizeof(struct cache_ent) * CACHE_N);
    for (u32 i = 0; i < fs->sb.bitmap_blocks; i++)
        if (raw_read(fs, fs->sb.bitmap_start + i, fs->bitmap + (usize)i * A2_BLOCK)) { kfree(fs->bitmap); kfree(fs->cache); kfree(fs); return NULL; }
    *was_dirty = fs->sb.dirty != 0;
    if (fs->sb.dirty)
        fsck_bitmap(fs);
    fs->sb.mount_count++;
    fs->sb.dirty = 0;
    fs->sb_dirty_on_disk = false;
    u8 sblk[A2_BLOCK];
    memset(sblk, 0, sizeof sblk);
    memcpy(sblk, &fs->sb, sizeof fs->sb);
    raw_write(fs, 0, sblk);
    return fs;
}

void aofs2_info(void *fsv, u32 *blocks, u32 *free_blocks, u32 *inodes, char *label, usize cap)
{
    struct a2fs *fs = fsv;
    *blocks = fs->sb.block_count;
    *free_blocks = fs->sb.free_blocks;
    *inodes = fs->sb.inode_count;
    strlcpy(label, fs->sb.label, cap);
}

/* ---------------------------------------------------------------- VFS-muveletek */
static int f_open(struct mount *m, const char *rel, u32 flags, void **node)
{
    struct a2fs *fs = m->fs;
    u32 parent;
    const char *last;
    usize len;
    u8 type = 0;
    u32 ino = walk(fs, rel, &parent, &last, &len, &type);
    if (!ino) {
        if (!(flags & O_CREATE) || !parent) return E_NOENT;
        ino = ialloc(fs, 1);
        if (!ino) return E_NOMEM;
        int e = dir_add(fs, parent, last, len, ino, 1);
        if (e) return e;
        type = 1;
    }
    if (type != 1) return E_ISDIR;
    if (flags & O_TRUNC) {
        struct a2_inode in;
        if (iread(fs, ino, &in)) return E_IO;
        ifree_blocks(fs, &in);
        in.mtime = pit_ticks();
        if (iwrite(fs, ino, &in)) return E_IO;
    }
    *node = (void *)(uptr)ino;
    return 0;
}

static isize f_read(struct mount *m, void *node, u64 pos, void *buf, usize n)
{
    return file_read(m->fs, (u32)(uptr)node, pos, buf, n);
}

static isize f_write(struct mount *m, void *node, u64 pos, const void *buf, usize n)
{
    return file_write(m->fs, (u32)(uptr)node, pos, buf, n);
}

static void f_close(struct mount *m, void *node) { (void)m; (void)node; }

static int f_list(struct mount *m, const char *rel, struct dirent *out, u32 max)
{
    struct a2fs *fs = m->fs;
    u8 type = 2;
    u32 dir = walk(fs, rel, NULL, NULL, NULL, &type);
    if (!dir) return E_NOENT;
    if (type != 2) return E_NOTDIR;
    struct a2_inode in;
    if (iread(fs, dir, &in)) return E_IO;
    u8 blk[A2_BLOCK];
    u32 n = 0;
    for (u32 idx = 0; (u64)idx * A2_BLOCK < in.size && n < max; idx++) {
        u32 b = imap(fs, &in, dir, idx, false);
        if (!b || bread(fs, b, blk)) continue;
        struct a2_dirent *de = (struct a2_dirent *)blk;
        for (u32 i = 0; i < DPB && n < max; i++) {
            if (!de[i].inode) continue;
            usize l = de[i].name_len;
            memcpy(out[n].name, de[i].name, l);
            out[n].name[l] = 0;
            out[n].type = de[i].type;
            struct a2_inode ci;
            out[n].size = iread(fs, de[i].inode, &ci) == 0 ? (u32)ci.size : 0;
            n++;
        }
    }
    return (int)n;
}

static int f_stat(struct mount *m, const char *rel, struct stat *st)
{
    struct a2fs *fs = m->fs;
    u8 type = 2;
    u32 ino = walk(fs, rel, NULL, NULL, NULL, &type);
    if (!ino) return E_NOENT;
    struct a2_inode in;
    if (iread(fs, ino, &in)) return E_IO;
    st->type = in.type;
    st->size = (u32)in.size;
    st->mtime = in.mtime;
    return 0;
}

static int f_mkdir(struct mount *m, const char *rel)
{
    struct a2fs *fs = m->fs;
    u32 parent;
    const char *last;
    usize len;
    if (walk(fs, rel, &parent, &last, &len, NULL)) return E_EXIST;
    if (!parent) return E_NOENT;
    u32 ino = ialloc(fs, 2);
    if (!ino) return E_NOMEM;
    return dir_add(fs, parent, last, len, ino, 2);
}

static int f_unlink(struct mount *m, const char *rel)
{
    struct a2fs *fs = m->fs;
    u32 parent;
    const char *last;
    usize len;
    u8 type = 0;
    u32 ino = walk(fs, rel, &parent, &last, &len, &type);
    if (!ino || !parent) return E_NOENT;
    if (ino == fs->sb.root_inode) return E_BUSY;
    if (type == 2 && !dir_empty(fs, ino)) return E_BUSY;
    struct a2_inode in;
    if (iread(fs, ino, &in)) return E_IO;
    int e = dir_remove(fs, parent, last, len);
    if (e) return e;
    ifree_blocks(fs, &in);
    memset(&in, 0, sizeof in);
    return iwrite(fs, ino, &in);
}

static int f_sync(struct mount *m)
{
    struct a2fs *fs = m->fs;
    if (!fs->sb_dirty_on_disk) return 0;
    fs->sb.dirty = 0;
    u8 blk[A2_BLOCK];
    memset(blk, 0, sizeof blk);
    memcpy(blk, &fs->sb, sizeof fs->sb);
    int e = raw_write(fs, 0, blk);
    if (e) return e;
    fs->sb_dirty_on_disk = false;
    blk_flush();
    return 0;
}

const struct fs_ops aofs2_ops = {
    .name = "aofs2",
    .open = f_open, .read = f_read, .write = f_write, .close = f_close,
    .list = f_list, .stat = f_stat, .mkdir = f_mkdir, .unlink = f_unlink, .sync = f_sync,
};
