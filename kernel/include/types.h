/* AO-OS alaptipusok. Nincs libc, nincs stdint.h: a fordito beepitett makroit hasznaljuk. */
#pragma once

typedef __UINT8_TYPE__  u8;
typedef __UINT16_TYPE__ u16;
typedef __UINT32_TYPE__ u32;
typedef __UINT64_TYPE__ u64;
typedef __INT8_TYPE__   i8;
typedef __INT16_TYPE__  i16;
typedef __INT32_TYPE__  i32;
typedef __INT64_TYPE__  i64;
typedef __UINTPTR_TYPE__ uptr;
typedef __SIZE_TYPE__   usize;
typedef __PTRDIFF_TYPE__ isize;

typedef _Bool bool;
#define true  1
#define false 0
#define NULL  ((void *)0)

#define PACKED      __attribute__((packed))
#define NORETURN    __attribute__((noreturn))
#define UNUSED      __attribute__((unused))
#define ALIGNED(n)  __attribute__((aligned(n)))

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define KiB (1024ULL)
#define MiB (1024ULL * 1024ULL)
