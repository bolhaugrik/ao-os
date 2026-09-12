#include "aofs.h"
#include "../task/task.h"
#include "../lib/string.h"

static const u8 *img;
static u32 img_size;
static const struct aofs_super *sb;
static const struct aofs_entry *tab;

bool aofs_mount(const void *image, u32 size)
{
    if (!image || size < 4096)
        return false;
    sb = image;
    if (sb->magic != AOFS_MAGIC || sb->version != 1)
        return false;
    if (4096 + (u64)sb->count * sizeof(struct aofs_entry) > size)
        return false;
    img = image;
    img_size = size;
    tab = (const struct aofs_entry *)(img + 4096);
    return true;
}

bool aofs_mounted(void) { return img != NULL; }
u32 aofs_count(void) { return img ? sb->count : 0; }
u32 aofs_image_size(void) { return img_size; }
const struct aofs_entry *aofs_entry(u32 i) { return (img && i < sb->count) ? &tab[i] : NULL; }

const struct aofs_entry *aofs_lookup(const char *path)
{
    if (!img)
        return NULL;
    while (*path == '/')
        path++;
    for (u32 i = 0; i < sb->count; i++)
        if (strcmp(tab[i].name, path) == 0)
            return &tab[i];
    return NULL;
}

const void *aofs_data(const struct aofs_entry *e)
{
    if (!e || e->offset + e->size > img_size)
        return NULL;
    return img + e->offset;
}

/* ---------------------------------------------------------------- VFS-illesztes */
static int a_open(struct mount *m, const char *rel, u32 flags, void **node)
{
    (void)m;
    if (flags & (O_WRITE | O_CREATE | O_TRUNC | O_APPEND)) return E_ROFS;
    const struct aofs_entry *e = aofs_lookup(rel);
    if (!e) return E_NOENT;
    if (e->type != AOFS_FILE) return E_ISDIR;
    *node = (void *)e;
    return 0;
}

static isize a_read(struct mount *m, void *node, u64 pos, void *buf, usize n)
{
    (void)m;
    const struct aofs_entry *e = node;
    if (pos >= e->size) return 0;
    usize c = e->size - pos < n ? e->size - pos : n;
    memcpy(buf, (const u8 *)aofs_data(e) + pos, c);
    return (isize)c;
}

static int a_list(struct mount *m, const char *rel, struct dirent *out, u32 max)
{
    (void)m;
    char prefix[96];
    strlcpy(prefix, rel, sizeof prefix);
    usize pl = strlen(prefix);
    if (pl) {
        const struct aofs_entry *d = aofs_lookup(rel);
        if (!d) return E_NOENT;
        if (d->type != AOFS_DIR) return E_NOTDIR;
        prefix[pl++] = '/';
        prefix[pl] = 0;
    }
    u32 n = 0;
    for (u32 i = 0; i < aofs_count() && n < max; i++) {
        const struct aofs_entry *e = aofs_entry(i);
        if (!str_starts(e->name, prefix)) continue;
        const char *rest = e->name + pl;
        if (!*rest) continue;
        bool nested = false;
        for (const char *p = rest; *p; p++) if (*p == '/') { nested = true; break; }
        if (nested) continue;
        strlcpy(out[n].name, rest, sizeof out[n].name);
        out[n].type = e->type;
        out[n].size = e->size;
        n++;
    }
    return (int)n;
}

static int a_stat(struct mount *m, const char *rel, struct stat *st)
{
    (void)m;
    const struct aofs_entry *e = aofs_lookup(rel);
    if (!e) return E_NOENT;
    st->type = e->type;
    st->size = e->size;
    st->mtime = 0;
    return 0;
}

const struct fs_ops aofs1_ops = {
    .name = "aofs1",
    .open = a_open, .read = a_read, .write = NULL, .close = NULL,
    .list = a_list, .stat = a_stat, .mkdir = NULL, .unlink = NULL, .sync = NULL,
};
