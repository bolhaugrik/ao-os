#include "vfs.h"
#include "../task/task.h"
#include "../mm/kheap.h"
#include "../lib/string.h"

static struct mount mounts[VFS_MOUNT_MAX];
static u32 nmounts;

void vfs_init(void)
{
    nmounts = 0;
}

int vfs_mount(const char *prefix, const struct fs_ops *ops, void *fs, bool ro)
{
    for (u32 i = 0; i < nmounts; i++)
        if (strcmp(mounts[i].prefix, prefix) == 0) {
            mounts[i].ops = ops; mounts[i].fs = fs; mounts[i].ro = ro;
            return 0;
        }
    if (nmounts >= VFS_MOUNT_MAX)
        return E_LIMIT;
    struct mount *m = &mounts[nmounts++];
    strlcpy(m->prefix, prefix, sizeof m->prefix);
    m->ops = ops;
    m->fs = fs;
    m->ro = ro;
    return 0;
}

int vfs_unmount(const char *prefix)
{
    for (u32 i = 0; i < nmounts; i++)
        if (strcmp(mounts[i].prefix, prefix) == 0) {
            if (mounts[i].ops->sync) mounts[i].ops->sync(&mounts[i]);
            for (u32 j = i + 1; j < nmounts; j++) mounts[j - 1] = mounts[j];
            nmounts--;
            return 0;
        }
    return E_NOENT;
}

const struct mount *vfs_mount_at(u32 i) { return i < nmounts ? &mounts[i] : NULL; }

/* leghosszabb prefix-illeszkedes; rel = a mount alatti relativ ut ('/' nelkul, "" = gyoker) */
static struct mount *resolve(const char *path, const char **rel)
{
    struct mount *best = NULL;
    usize best_len = 0;
    for (u32 i = 0; i < nmounts; i++) {
        const char *p = mounts[i].prefix;
        usize pl = strlen(p);
        if (pl == 1 && p[0] == '/') {
            if (!best) { best = &mounts[i]; best_len = 1; }
            continue;
        }
        if (strncmp(path, p, pl) == 0 && (path[pl] == 0 || path[pl] == '/') && pl > best_len) {
            best = &mounts[i];
            best_len = pl;
        }
    }
    if (!best) return NULL;
    const char *r = path + (best_len == 1 ? 1 : best_len);
    while (*r == '/') r++;
    *rel = r;
    return best;
}

bool vfs_canon(const char *cwd, const char *in, char *out, usize cap)
{
    char tmp[VFS_PATH_MAX * 2];
    if (in[0] == '/') {
        strlcpy(tmp, in, sizeof tmp);
    } else {
        usize cl = strlen(cwd);
        if (cl + 1 + strlen(in) + 1 > sizeof tmp) return false;
        memcpy(tmp, cwd, cl);
        tmp[cl] = '/';
        strcpy(tmp + cl + 1, in);
    }
    /* komponensenkent epitjuk */
    usize o = 0;
    out[o++] = '/';
    const char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        usize len = (usize)(p - start);
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (o > 1) {
                o--;                          /* a zaro '/' ele */
                while (o > 1 && out[o - 1] != '/') o--;
            }
            continue;
        }
        if (o + len + 1 >= cap) return false;
        memcpy(out + o, start, len);
        o += len;
        out[o++] = '/';
    }
    if (o > 1) o--;                            /* zaro '/' levagasa */
    out[o] = 0;
    return true;
}

int vfs_open(const char *path, u32 flags, struct handle *h)
{
    const char *rel;
    struct mount *m = resolve(path, &rel);
    if (!m) return E_NOENT;
    if ((flags & (O_WRITE | O_CREATE | O_TRUNC | O_APPEND)) && m->ro) return E_ROFS;
    if (!m->ops->open) return E_NOSYS;
    void *node = NULL;
    int e = m->ops->open(m, rel, flags, &node);
    if (e) return e;
    struct vfile *f = kzalloc(sizeof *f);
    f->m = m;
    f->node = node;
    f->flags = flags;
    h->type = H_FILE;
    h->obj = f;
    h->pos = 0;
    if (flags & O_APPEND) {
        struct stat st;
        if (m->ops->stat && m->ops->stat(m, rel, &st) == 0)
            h->pos = st.size;
    }
    return 0;
}

isize vfs_read(struct handle *h, void *buf, usize n)
{
    struct vfile *f = h->obj;
    if (!(f->flags & O_READ)) return E_BADF;
    isize r = f->m->ops->read ? f->m->ops->read(f->m, f->node, h->pos, buf, n) : E_NOSYS;
    if (r > 0) h->pos += (u64)r;
    return r;
}

isize vfs_write(struct handle *h, const void *buf, usize n)
{
    struct vfile *f = h->obj;
    if (!(f->flags & O_WRITE)) return E_BADF;
    isize r = f->m->ops->write ? f->m->ops->write(f->m, f->node, h->pos, buf, n) : E_ROFS;
    if (r > 0) h->pos += (u64)r;
    return r;
}

void vfs_close(struct handle *h)
{
    struct vfile *f = h->obj;
    if (!f) return;
    if (f->m->ops->close) f->m->ops->close(f->m, f->node);
    kfree(f);
    h->obj = NULL;
    h->type = H_NONE;
}

int vfs_list(const char *path, struct dirent *out, u32 max)
{
    const char *rel;
    struct mount *m = resolve(path, &rel);
    if (!m || !m->ops->list) return E_NOENT;
    int n = m->ops->list(m, rel, out, max);
    if (n < 0) return n;
    /* a mount-pontok mint konyvtarak a szulojukben */
    for (u32 i = 0; i < nmounts && (u32)n < max; i++) {
        const char *p = mounts[i].prefix;
        if (p[0] == '/' && p[1] == 0) continue;
        usize pl = strlen(path);
        bool parent_is_root = (pl == 1);
        const char *last = p + 1;
        usize plen = strlen(p);
        if (parent_is_root) {
            for (const char *q = p + 1; *q; q++) if (*q == '/') { last = NULL; break; }
        } else {
            if (!(strncmp(p, path, pl) == 0 && p[pl] == '/')) continue;
            last = p + pl + 1;
            for (const char *q = last; *q; q++) if (*q == '/') { last = NULL; break; }
        }
        (void)plen;
        if (!last) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) if (strcmp(out[k].name, last) == 0) dup = true;
        if (dup) continue;
        strlcpy(out[n].name, last, sizeof out[n].name);
        out[n].type = 2;
        out[n].size = 0;
        n++;
    }
    return n;
}

int vfs_stat(const char *path, struct stat *st)
{
    const char *rel;
    struct mount *m = resolve(path, &rel);
    if (!m) return E_NOENT;
    if (!*rel) { st->type = 2; st->size = 0; st->mtime = 0; return 0; }
    if (!m->ops->stat) return E_NOSYS;
    return m->ops->stat(m, rel, st);
}

int vfs_mkdir(const char *path)
{
    const char *rel;
    struct mount *m = resolve(path, &rel);
    if (!m) return E_NOENT;
    if (m->ro) return E_ROFS;
    if (!m->ops->mkdir) return E_NOSYS;
    return m->ops->mkdir(m, rel);
}

int vfs_unlink(const char *path)
{
    const char *rel;
    struct mount *m = resolve(path, &rel);
    if (!m) return E_NOENT;
    if (m->ro) return E_ROFS;
    if (!m->ops->unlink) return E_NOSYS;
    return m->ops->unlink(m, rel);
}

int vfs_sync(void)
{
    int rc = 0;
    for (u32 i = 0; i < nmounts; i++)
        if (mounts[i].ops->sync) {
            int e = mounts[i].ops->sync(&mounts[i]);
            if (e) rc = e;
        }
    return rc;
}

int vfs_read_all(const char *path, void **buf, usize *size)
{
    struct stat st;
    int e = vfs_stat(path, &st);
    if (e) return e;
    if (st.type != 1) return E_ISDIR;
    struct handle h;
    e = vfs_open(path, O_READ, &h);
    if (e) return e;
    u8 *data = kmalloc(st.size ? st.size : 1);
    usize got = 0;
    while (got < st.size) {
        isize r = vfs_read(&h, data + got, st.size - got);
        if (r <= 0) break;
        got += (usize)r;
    }
    vfs_close(&h);
    *buf = data;
    *size = got;
    return 0;
}

int vfs_write_all(const char *path, const void *buf, usize size)
{
    struct handle h;
    int e = vfs_open(path, O_WRITE | O_CREATE | O_TRUNC, &h);
    if (e) return e;
    usize put = 0;
    while (put < size) {
        isize r = vfs_write(&h, (const u8 *)buf + put, size - put);
        if (r <= 0) { vfs_close(&h); return r < 0 ? (int)r : E_IO; }
        put += (usize)r;
    }
    vfs_close(&h);
    return 0;
}
