/* MicroPython AO-OS port: beallitasok. Az "extra" szint (unicode str, help, input, json, re, random, heapq,
 * os, errno, sys.stdin/stdout, reszletes hibauzenetek) plusz dupla pontossagu float es nagy egeszek (mpz).
 * Amit a netbookon nem tudunk vagy nem kell (aszinkron, select, tomorites, halozat, hardver-modulok),
 * az kikapcsolva. A fajlrendszer a sajat VfsAO (port/vfs_ao.c) a rendszerhivasainkra. */
#pragma once
#include <stdint.h>

#define MICROPY_CONFIG_ROM_LEVEL            (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)

#define MICROPY_ENABLE_COMPILER             (1)
#define MICROPY_ENABLE_GC                   (1)
#define MICROPY_ENABLE_FINALISER            (1)
#define MICROPY_STACK_CHECK                 (1)
#define MICROPY_KBD_EXCEPTION               (1)
#define MICROPY_HELPER_REPL                 (1)
#define MICROPY_REPL_AUTO_INDENT            (1)
#define MICROPY_USE_READLINE_HISTORY        (1)
#define MICROPY_READLINE_HISTORY_SIZE       (32)
#define MICROPY_HAL_HAS_VT100               (1)
#define MICROPY_ENABLE_SOURCE_LINE          (1)
#define MICROPY_ERROR_REPORTING             (MICROPY_ERROR_REPORTING_DETAILED)
#define MICROPY_WARNINGS                    (1)
#define MICROPY_FLOAT_IMPL                  (MICROPY_FLOAT_IMPL_DOUBLE)
#define MICROPY_FLOAT_USE_NATIVE_FLT16      (0)     /* nincs compiler-rt: a felpontossagu float szoftveres */
#define MICROPY_LONGINT_IMPL                (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_USE_INTERNAL_ERRNO          (1)
#define MICROPY_USE_INTERNAL_PRINTF         (1)
#define MICROPY_ALLOC_PATH_MAX              (256)
#define MICROPY_MODULE_BUILTIN_INIT         (1)
#define MICROPY_PY_BUILTINS_HELP            (1)
#define MICROPY_PY_BUILTINS_HELP_TEXT       ao_help_text
#define MICROPY_PY_BUILTINS_HELP_MODULES    (1)
#define MICROPY_PY_MICROPYTHON_MEM_INFO     (1)

/* fajlrendszer: VfsAO a gyokeren, import es open() rajta at */
#define MICROPY_VFS                         (1)
#define MICROPY_VFS_WRITABLE                (1)
#define MICROPY_READER_VFS                  (1)
#define MICROPY_PY_IO                       (1)
#define MICROPY_PY_IO_IOBASE                (1)
#define MICROPY_PY_SYS_STDFILES             (1)
#define MICROPY_PY_SYS_STDIO_BUFFER         (0)
#define MICROPY_PY_OS                       (1)
#define MICROPY_PY_OS_UNAME                 (1)
#define MICROPY_PY_OS_UNAME_RELEASE_DYNAMIC (0)
#define MICROPY_PY_OS_SYNC                  (0)
#define MICROPY_PY_OS_STATVFS               (0)
#define MICROPY_PY_OS_URANDOM               (0)
#define MICROPY_PY_OS_ERRNO                 (0)
#define MICROPY_PY_OS_DUPTERM               (0)
#define MICROPY_PY_OS_GETENV_PUTENV_UNSETENV (0)
#define MICROPY_PY_OS_SYSTEM                (0)

/* modulok */
#define MICROPY_PY_TIME                     (1)
#define MICROPY_PY_TIME_TIME_TIME_NS        (0)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (0)
#define MICROPY_PY_RANDOM                   (1)
#define MICROPY_PY_RANDOM_EXTRA_FUNCS       (1)
unsigned long ao_random_seed(void);       /* port/libc_ao.c: rdtsc */
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC    (ao_random_seed())
#define MICROPY_PY_RE                       (1)
#define MICROPY_PY_RE_SUB                   (1)
#define MICROPY_PY_JSON                     (1)
#define MICROPY_PY_HEAPQ                    (1)
#define MICROPY_PY_BINASCII                 (1)
#define MICROPY_PY_BINASCII_CRC32           (0)
#define MICROPY_PY_CMATH                    (1)
#define MICROPY_PY_ERRNO                    (1)
#define MICROPY_PY_HASHLIB                  (0)
#define MICROPY_PY_CRYPTOLIB                (0)
#define MICROPY_PY_ASYNCIO                  (0)
#define MICROPY_PY_SELECT                   (0)
#define MICROPY_PY_DEFLATE                  (0)
#define MICROPY_PY_UCTYPES                  (0)
#define MICROPY_PY_PLATFORM                 (0)
#define MICROPY_PY_VFS                      (0)
#define MICROPY_PY_MACHINE                  (0)
#define MICROPY_PY_NETWORK                  (0)
#define MICROPY_PY_SOCKET                   (0)
#define MICROPY_PY_SSL                      (0)
#define MICROPY_PY_WEBSOCKET                (0)
#define MICROPY_PY_WEBREPL                  (0)
#define MICROPY_PY_FRAMEBUF                 (0)
#define MICROPY_PY_BTREE                    (0)
#define MICROPY_PY_ONEWIRE                  (0)
#define MICROPY_PY_BLUETOOTH                (0)
#define MICROPY_PY_LWIP                     (0)
#define MICROPY_PY_MARSHAL                  (0)
#define MICROPY_PY_THREAD                   (0)
#define MICROPY_PY_SYS_SETTRACE             (0)

#define MICROPY_PY_SYS_PLATFORM             "ao-os"
#define MICROPY_HW_BOARD_NAME               "AO-OS"
#define MICROPY_HW_MCU_NAME                 "x86-64"

/* gepi tipusok */
typedef intptr_t mp_int_t;      // must be pointer size
typedef uintptr_t mp_uint_t;    // must be pointer size
typedef long mp_off_t;

#include <alloca.h>

#define MP_STATE_PORT MP_STATE_VM
#define MICROPY_MPHALPORT_H "mphalport.h"
