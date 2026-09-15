/* VfsAO: a MicroPython VFS-protokollja az AO-OS rendszerhivasain. A gyokerre csatolva ad open()-t,
 * os.listdir/mkdir/remove/rename/stat/chdir/getcwd-t es a fajlbol importot. A relativ utvonalakat a
 * kernel a task munkakonyvtarahoz kepest oldja fel (SYS_CHDIR/SYS_GETCWD), a capability-ellenorzes
 * is a kernelben van: amit a python.cap nem enged, arra OSError(EACCES) jon. */
#include "aolib.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "extmod/vfs.h"
#include "vfs_ao.h"

typedef struct _mp_obj_vfs_ao_t {
    mp_obj_base_t base;
} mp_obj_vfs_ao_t;

typedef struct _mp_obj_vfs_ao_file_t {
    mp_obj_base_t base;
    int fd;
    u64 pos, size;
} mp_obj_vfs_ao_file_t;

/* AO hibakod -> MicroPython errno */
static int mp_err(int e)
{
    switch (e) {
    case E_NOENT: return MP_ENOENT;
    case E_EXIST: return MP_EEXIST;
    case E_ISDIR: return MP_EISDIR;
    case E_NOTDIR: return MP_ENOTDIR;
    case E_ROFS: return MP_EROFS;
    case E_CAP: return MP_EACCES;
    case E_NOMEM: return MP_ENOMEM;
    case E_BADF: return MP_EBADF;
    case E_LIMIT: return MP_ENOSPC;
    case E_INVAL: return MP_EINVAL;
    default: return MP_EIO;
    }
}

/* ---------------------------------------------------------------- fajl */
mp_obj_t mp_vfs_ao_file_open(const mp_obj_type_t *type, mp_obj_t path_in, mp_obj_t mode_in)
{
    const char *mode = mp_obj_str_get_str(mode_in);
    u32 flags = 0;
    bool append = false;
    for (; *mode; mode++) {
        switch (*mode) {
        case 'r': flags |= O_READ; break;
        case 'w': flags |= O_WRITE | O_CREATE | O_TRUNC; break;
        case 'a': flags |= O_WRITE | O_CREATE | O_APPEND; append = true; break;
        case '+': flags |= O_READ | O_WRITE; break;
        case 'b': type = &mp_type_vfs_ao_fileio; break;
        case 't': type = &mp_type_vfs_ao_textio; break;
        }
    }
    if (!(flags & (O_READ | O_WRITE))) flags |= O_READ;
    const char *path = mp_obj_str_get_str(path_in);
    struct stat st;
    bool exists = ao_stat(path, &st) == 0;
    if (exists && st.type == 2) mp_raise_OSError(MP_EISDIR);
    int fd = ao_open(path, flags);
    if (fd < 0) mp_raise_OSError(mp_err(fd));
    mp_obj_vfs_ao_file_t *o = mp_obj_malloc_with_finaliser(mp_obj_vfs_ao_file_t, type);
    o->fd = fd;
    o->size = (exists && !(flags & O_TRUNC)) ? st.size : 0;
    o->pos = append ? o->size : 0;
    return MP_OBJ_FROM_PTR(o);
}

static void file_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    (void)kind;
    mp_obj_vfs_ao_file_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<io.%s %d>", mp_obj_get_type_str(self_in), self->fd);
}

static mp_uint_t file_read(mp_obj_t o_in, void *buf, mp_uint_t size, int *errcode)
{
    mp_obj_vfs_ao_file_t *f = MP_OBJ_TO_PTR(o_in);
    if (f->fd < 0) { *errcode = MP_EBADF; return MP_STREAM_ERROR; }
    isize r = ao_read(f->fd, buf, size);
    if (r < 0) { *errcode = mp_err((int)r); return MP_STREAM_ERROR; }
    f->pos += (u64)r;
    return (mp_uint_t)r;
}

static mp_uint_t file_write(mp_obj_t o_in, const void *buf, mp_uint_t size, int *errcode)
{
    mp_obj_vfs_ao_file_t *f = MP_OBJ_TO_PTR(o_in);
    if (f->fd < 0) { *errcode = MP_EBADF; return MP_STREAM_ERROR; }
    if (f->fd == 1) { mp_hal_stdout_tx_strn(buf, size); return size; }
    isize w = ao_write(f->fd, buf, size);
    if (w < 0) { *errcode = mp_err((int)w); return MP_STREAM_ERROR; }
    f->pos += (u64)w;
    if (f->pos > f->size) f->size = f->pos;
    return (mp_uint_t)w;
}

static mp_uint_t file_ioctl(mp_obj_t o_in, mp_uint_t request, uintptr_t arg, int *errcode)
{
    mp_obj_vfs_ao_file_t *f = MP_OBJ_TO_PTR(o_in);
    if (request != MP_STREAM_CLOSE && f->fd < 0) { *errcode = MP_EBADF; return MP_STREAM_ERROR; }
    switch (request) {
    case MP_STREAM_FLUSH:
        return 0;
    case MP_STREAM_SEEK: {
        struct mp_stream_seek_t *s = (struct mp_stream_seek_t *)arg;
        i64 np = s->whence == MP_SEEK_SET ? s->offset : s->whence == MP_SEEK_CUR ? (i64)f->pos + s->offset : (i64)f->size + s->offset;
        if (np < 0) { *errcode = MP_EINVAL; return MP_STREAM_ERROR; }
        i64 r = ao_syscall(SYS_SEEK, (u64)f->fd, (u64)np, 0, 0);
        if (r < 0) { *errcode = mp_err((int)r); return MP_STREAM_ERROR; }
        f->pos = (u64)np;
        s->offset = np;
        return 0;
    }
    case MP_STREAM_CLOSE:
        if (f->fd > 1) ao_close(f->fd);        /* a konzol (0, 1) nyitva marad */
        f->fd = -1;
        return 0;
    case MP_STREAM_GET_FILENO:
        return (mp_uint_t)f->fd;
    default:
        *errcode = MP_EINVAL;
        return MP_STREAM_ERROR;
    }
}

static mp_obj_t file_fileno(mp_obj_t self_in)
{
    mp_obj_vfs_ao_file_t *f = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(f->fd);
}
static MP_DEFINE_CONST_FUN_OBJ_1(file_fileno_obj, file_fileno);

static const mp_rom_map_elem_t file_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_fileno), MP_ROM_PTR(&file_fileno_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&mp_stream_seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&mp_stream_tell_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(file_locals_dict, file_locals_dict_table);

static const mp_stream_p_t fileio_stream_p = {
    .read = file_read,
    .write = file_write,
    .ioctl = file_ioctl,
};

MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_vfs_ao_fileio,
    MP_QSTR_FileIO,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    print, file_print,
    protocol, &fileio_stream_p,
    locals_dict, &file_locals_dict
    );

static const mp_stream_p_t textio_stream_p = {
    .read = file_read,
    .write = file_write,
    .ioctl = file_ioctl,
    .is_text = true,
};

MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_vfs_ao_textio,
    MP_QSTR_TextIOWrapper,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    print, file_print,
    protocol, &textio_stream_p,
    locals_dict, &file_locals_dict
    );

/* sys.stdin / sys.stdout / sys.stderr: a konzol (0 = bemenet, 1 = kimenet) */
mp_obj_vfs_ao_file_t mp_sys_stdin_obj = { { &mp_type_vfs_ao_textio }, 0, 0, 0 };
mp_obj_vfs_ao_file_t mp_sys_stdout_obj = { { &mp_type_vfs_ao_textio }, 1, 0, 0 };
mp_obj_vfs_ao_file_t mp_sys_stderr_obj = { { &mp_type_vfs_ao_textio }, 1, 0, 0 };

/* ---------------------------------------------------------------- VFS */
static mp_import_stat_t vfs_ao_import_stat(void *self_in, const char *path)
{
    (void)self_in;
    struct stat st;
    if (ao_stat(path, &st) != 0) return MP_IMPORT_STAT_NO_EXIST;
    return st.type == 2 ? MP_IMPORT_STAT_DIR : MP_IMPORT_STAT_FILE;
}

static mp_obj_t vfs_ao_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    (void)args;
    mp_arg_check_num(n_args, n_kw, 0, 1, false);
    mp_obj_vfs_ao_t *vfs = mp_obj_malloc(mp_obj_vfs_ao_t, type);
    return MP_OBJ_FROM_PTR(vfs);
}

static mp_obj_t vfs_ao_mount(mp_obj_t self_in, mp_obj_t readonly, mp_obj_t mkfs)
{
    (void)self_in; (void)readonly;
    if (mp_obj_is_true(mkfs)) mp_raise_OSError(MP_EPERM);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(vfs_ao_mount_obj, vfs_ao_mount);

static mp_obj_t vfs_ao_umount(mp_obj_t self_in) { (void)self_in; return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_1(vfs_ao_umount_obj, vfs_ao_umount);

static mp_obj_t vfs_ao_open(mp_obj_t self_in, mp_obj_t path_in, mp_obj_t mode_in)
{
    (void)self_in;
    return mp_vfs_ao_file_open(&mp_type_vfs_ao_textio, path_in, mode_in);
}
static MP_DEFINE_CONST_FUN_OBJ_3(vfs_ao_open_obj, vfs_ao_open);

static mp_obj_t vfs_ao_chdir(mp_obj_t self_in, mp_obj_t path_in)
{
    (void)self_in;
    const char *path = mp_obj_str_get_str(path_in);
    i64 e = ao_syscall(SYS_CHDIR, (u64)(uptr)path, 0, 0, 0);
    if (e < 0) mp_raise_OSError(mp_err((int)e));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vfs_ao_chdir_obj, vfs_ao_chdir);

static mp_obj_t vfs_ao_getcwd(mp_obj_t self_in)
{
    (void)self_in;
    char buf[MICROPY_ALLOC_PATH_MAX + 1];
    i64 e = ao_syscall(SYS_GETCWD, (u64)(uptr)buf, sizeof buf, 0, 0);
    if (e < 0) mp_raise_OSError(mp_err((int)e));
    return mp_obj_new_str_from_cstr(buf);
}
static MP_DEFINE_CONST_FUN_OBJ_1(vfs_ao_getcwd_obj, vfs_ao_getcwd);

typedef struct _vfs_ao_ilistdir_it_t {
    mp_obj_base_t base;
    mp_fun_1_t iternext;
    struct dirent *ents;
    int n, i;
    bool is_str;
} vfs_ao_ilistdir_it_t;

static mp_obj_t vfs_ao_ilistdir_it_iternext(mp_obj_t self_in)
{
    vfs_ao_ilistdir_it_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->i >= self->n) return MP_OBJ_STOP_ITERATION;
    const struct dirent *d = &self->ents[self->i++];
    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(3, NULL));
    t->items[0] = self->is_str ? mp_obj_new_str_from_cstr(d->name) : mp_obj_new_bytes((const byte *)d->name, strlen(d->name));
    t->items[1] = MP_OBJ_NEW_SMALL_INT(d->type == 2 ? MP_S_IFDIR : MP_S_IFREG);
    t->items[2] = MP_OBJ_NEW_SMALL_INT(0);
    return MP_OBJ_FROM_PTR(t);
}

static mp_obj_t vfs_ao_ilistdir(mp_obj_t self_in, mp_obj_t path_in)
{
    (void)self_in;
    vfs_ao_ilistdir_it_t *iter = mp_obj_malloc(vfs_ao_ilistdir_it_t, &mp_type_polymorph_iter);
    iter->iternext = vfs_ao_ilistdir_it_iternext;
    iter->is_str = mp_obj_get_type(path_in) == &mp_type_str;
    const char *path = mp_obj_str_get_str(path_in);
    if (path[0] == 0) path = ".";
    iter->ents = m_new(struct dirent, 64);
    int n = ao_list(path, iter->ents, 64);
    if (n < 0) mp_raise_OSError(mp_err(n));
    iter->n = n;
    iter->i = 0;
    return MP_OBJ_FROM_PTR(iter);
}
static MP_DEFINE_CONST_FUN_OBJ_2(vfs_ao_ilistdir_obj, vfs_ao_ilistdir);

static mp_obj_t vfs_ao_mkdir(mp_obj_t self_in, mp_obj_t path_in)
{
    (void)self_in;
    int e = ao_mkdir(mp_obj_str_get_str(path_in));
    if (e) mp_raise_OSError(mp_err(e));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vfs_ao_mkdir_obj, vfs_ao_mkdir);

static mp_obj_t vfs_ao_remove(mp_obj_t self_in, mp_obj_t path_in)
{
    (void)self_in;
    int e = ao_unlink(mp_obj_str_get_str(path_in));
    if (e) mp_raise_OSError(mp_err(e));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vfs_ao_remove_obj, vfs_ao_remove);

/* az AOFS-ben nincs atnevezes: masolas + torles */
static mp_obj_t vfs_ao_rename(mp_obj_t self_in, mp_obj_t old_in, mp_obj_t new_in)
{
    (void)self_in;
    const char *src = mp_obj_str_get_str(old_in), *dst = mp_obj_str_get_str(new_in);
    int in = ao_open(src, O_READ);
    if (in < 0) mp_raise_OSError(mp_err(in));
    int out = ao_open(dst, O_WRITE | O_CREATE | O_TRUNC);
    if (out < 0) { ao_close(in); mp_raise_OSError(mp_err(out)); }
    u8 *blk = m_new(u8, 16384);
    int e = 0;
    for (;;) {
        isize r = ao_read(in, blk, 16384);
        if (r < 0) { e = (int)r; break; }
        if (r == 0) break;
        isize w = ao_write(out, blk, (usize)r);
        if (w != r) { e = w < 0 ? (int)w : E_IO; break; }
    }
    m_del(u8, blk, 16384);
    ao_close(in);
    ao_close(out);
    if (e) mp_raise_OSError(mp_err(e));
    e = ao_unlink(src);
    if (e) mp_raise_OSError(mp_err(e));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(vfs_ao_rename_obj, vfs_ao_rename);

static mp_obj_t vfs_ao_stat(mp_obj_t self_in, mp_obj_t path_in)
{
    (void)self_in;
    struct stat st;
    int e = ao_stat(mp_obj_str_get_str(path_in), &st);
    if (e) mp_raise_OSError(mp_err(e));
    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(10, NULL));
    t->items[0] = MP_OBJ_NEW_SMALL_INT(st.type == 2 ? MP_S_IFDIR : MP_S_IFREG);
    for (int i = 1; i < 10; i++) t->items[i] = MP_OBJ_NEW_SMALL_INT(0);
    t->items[6] = mp_obj_new_int_from_uint(st.size);
    return MP_OBJ_FROM_PTR(t);
}
static MP_DEFINE_CONST_FUN_OBJ_2(vfs_ao_stat_obj, vfs_ao_stat);

static const mp_rom_map_elem_t vfs_ao_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_mount), MP_ROM_PTR(&vfs_ao_mount_obj) },
    { MP_ROM_QSTR(MP_QSTR_umount), MP_ROM_PTR(&vfs_ao_umount_obj) },
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&vfs_ao_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_chdir), MP_ROM_PTR(&vfs_ao_chdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_getcwd), MP_ROM_PTR(&vfs_ao_getcwd_obj) },
    { MP_ROM_QSTR(MP_QSTR_ilistdir), MP_ROM_PTR(&vfs_ao_ilistdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&vfs_ao_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&vfs_ao_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_rename), MP_ROM_PTR(&vfs_ao_rename_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&vfs_ao_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&vfs_ao_stat_obj) },
};
static MP_DEFINE_CONST_DICT(vfs_ao_locals_dict, vfs_ao_locals_dict_table);

static const mp_vfs_proto_t vfs_ao_proto = {
    .import_stat = vfs_ao_import_stat,
};

MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_vfs_ao,
    MP_QSTR_VfsAO,
    MP_TYPE_FLAG_NONE,
    make_new, vfs_ao_make_new,
    protocol, &vfs_ao_proto,
    locals_dict, &vfs_ao_locals_dict
    );
