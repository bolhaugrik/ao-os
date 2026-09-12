#include "ramfs.h"
#include "../mm/kheap.h"
#include "../lib/string.h"

struct rnode {
    char name[NAME_MAX + 1];
    u32 type;               /* 1 fajl, 2 konyvtar */
    u8 *data;
    usize size, cap;
    ramfs_gen_fn gen;
    struct rnode *parent, *child, *next;
    u32 refs;
};

struct ramfs {
    struct rnode root;
};

void *ramfs_create(void)
{
    struct ramfs *fs = kzalloc(sizeof *fs);
    fs->root.type = 2;
    return fs;
}

static struct rnode *find_child(struct rnode *dir, const char *name, usize len)
{
    for (struct rnode *c = dir->child; c; c = c->next)
        if (strlen(c->name) == len && memcmp(c->name, name, len) == 0)
            return c;
    return NULL;
}

/* rel utvonal feloldasa; ha parent_out != NULL, az utolso komponens szulojet es nevet adja */
static struct rnode *walk(struct ramfs *fs, const char *rel, struct rnode **parent_out, const char **last, usize *last_len)
{
    struct rnode *cur = &fs->root;
    const char *p = rel;
    if (parent_out) { *parent_out = NULL; *last = ""; *last_len = 0; }
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        usize len = (usize)(p - start);
        while (*p == '/') p++;
        bool last_comp = (*p == 0);
        if (last_comp) {
            if (parent_out) {
                *parent_out = cur;
                *last = start;
                *last_len = len;
            }
            return find_child(cur, start, len);
        }
        cur = find_child(cur, start, len);
        if (!cur || cur->type != 2)
            return NULL;
    }
    return cur;
}

static int r_open(struct mount *m, const char *rel, u32 flags, void **node)
{
    struct ramfs *fs = m->fs;
    struct rnode *parent;
    const char *last;
    usize len;
    struct rnode *n = walk(fs, rel, &parent, &last, &len);
    if (!n) {
        if (!(flags & O_CREATE) || !parent) return E_NOENT;
        if (len == 0 || len > NAME_MAX) return E_INVAL;
        n = kzalloc(sizeof *n);
        memcpy(n->name, last, len);
        n->type = 1;
        n->parent = parent;
        n->next = parent->child;
        parent->child = n;
    }
    if (n->type != 1) return E_ISDIR;
    if ((flags & O_TRUNC) && !n->gen) n->size = 0;
    n->refs++;
    *node = n;
    return 0;
}

static isize r_read(struct mount *m, void *node, u64 pos, void *buf, usize n)
{
    (void)m;
    struct rnode *f = node;
    if (f->gen) {
        char *tmp = kmalloc(8192);
        usize len = f->gen(tmp, 8192);
        if (pos >= len) { kfree(tmp); return 0; }
        usize c = len - pos < n ? len - pos : n;
        memcpy(buf, tmp + pos, c);
        kfree(tmp);
        return (isize)c;
    }
    if (pos >= f->size) return 0;
    usize c = f->size - pos < n ? f->size - pos : n;
    memcpy(buf, f->data + pos, c);
    return (isize)c;
}

static isize r_write(struct mount *m, void *node, u64 pos, const void *buf, usize n)
{
    (void)m;
    struct rnode *f = node;
    if (f->gen) return E_ROFS;
    usize end = pos + n;
    if (end > f->cap) {
        usize ncap = f->cap ? f->cap : 256;
        while (ncap < end) ncap *= 2;
        if (ncap > 4 * MiB) return E_LIMIT;
        u8 *nd = kmalloc(ncap);
        if (f->data) { memcpy(nd, f->data, f->size); kfree(f->data); }
        f->data = nd;
        f->cap = ncap;
    }
    if (pos > f->size) memset(f->data + f->size, 0, pos - f->size);
    memcpy(f->data + pos, buf, n);
    if (end > f->size) f->size = end;
    return (isize)n;
}

static void r_close(struct mount *m, void *node)
{
    (void)m;
    struct rnode *f = node;
    if (f->refs) f->refs--;
}

static int r_list(struct mount *m, const char *rel, struct dirent *out, u32 max)
{
    struct rnode *d = walk(m->fs, rel, NULL, NULL, NULL);
    if (!d) return E_NOENT;
    if (d->type != 2) return E_NOTDIR;
    u32 n = 0;
    for (struct rnode *c = d->child; c && n < max; c = c->next, n++) {
        strlcpy(out[n].name, c->name, sizeof out[n].name);
        out[n].type = c->type;
        out[n].size = (u32)c->size;
    }
    return (int)n;
}

static int r_stat(struct mount *m, const char *rel, struct stat *st)
{
    struct rnode *n = walk(m->fs, rel, NULL, NULL, NULL);
    if (!n) return E_NOENT;
    st->type = n->type;
    st->size = (u32)n->size;
    st->mtime = 0;
    if (n->gen) {
        char *tmp = kmalloc(8192);
        st->size = (u32)n->gen(tmp, 8192);
        kfree(tmp);
    }
    return 0;
}

static int r_mkdir(struct mount *m, const char *rel)
{
    struct rnode *parent;
    const char *last;
    usize len;
    struct rnode *n = walk(m->fs, rel, &parent, &last, &len);
    if (n) return E_EXIST;
    if (!parent || len == 0 || len > NAME_MAX) return E_NOENT;
    n = kzalloc(sizeof *n);
    memcpy(n->name, last, len);
    n->type = 2;
    n->parent = parent;
    n->next = parent->child;
    parent->child = n;
    return 0;
}

static int r_unlink(struct mount *m, const char *rel)
{
    struct rnode *parent;
    const char *last;
    usize len;
    struct rnode *n = walk(m->fs, rel, &parent, &last, &len);
    if (!n || !parent) return E_NOENT;
    if (n->type == 2 && n->child) return E_BUSY;
    if (n->refs) return E_BUSY;
    struct rnode **pp = &parent->child;
    while (*pp && *pp != n) pp = &(*pp)->next;
    if (*pp) *pp = n->next;
    if (n->data) kfree(n->data);
    kfree(n);
    return 0;
}

int ramfs_add_generated(void *fsv, const char *path, ramfs_gen_fn fn)
{
    struct ramfs *fs = fsv;
    struct rnode *parent;
    const char *last;
    usize len;
    while (*path == '/') path++;
    struct rnode *n = walk(fs, path, &parent, &last, &len);
    if (n || !parent) return E_EXIST;
    n = kzalloc(sizeof *n);
    memcpy(n->name, last, len);
    n->type = 1;
    n->gen = fn;
    n->parent = parent;
    n->next = parent->child;
    parent->child = n;
    return 0;
}

const struct fs_ops ramfs_ops = {
    .name = "ramfs",
    .open = r_open, .read = r_read, .write = r_write, .close = r_close,
    .list = r_list, .stat = r_stat, .mkdir = r_mkdir, .unlink = r_unlink, .sync = NULL,
};
