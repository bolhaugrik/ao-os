/* AO-OS: a MicroPython a sajat belso errno-tablajat hasznalja (MICROPY_USE_INTERNAL_ERRNO); ez a fejlec csak
 * azert van, hogy a <errno.h> include-ok leforduljanak */
#pragma once
#define EPERM 1
#define ENOENT 2
#define EIO 5
#define EBADF 9
#define ENOMEM 12
#define EACCES 13
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENOSPC 28
#define EROFS 30
