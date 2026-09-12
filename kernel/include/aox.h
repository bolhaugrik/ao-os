/* AOX v1: lapos futtathato kep. Phase 1-ben kernel-modu fuggvenykent fut (megbizhato kod),
 * Phase 2-ben ring 3-ba kerul ugyanezzel a fejleccel.
 *
 * A kep pozicio-fuggetlen (RIP-relativ), a betoltesi cim tetszoleges. */
#pragma once
#include "types.h"

#define AOX_MAGIC 0x31584F41u   /* 'AOX1' */

struct aox_header {
    u32 magic;
    u32 version;
    u32 entry;        /* eltolas a kep elejetol */
    u32 load_size;    /* fajl merete (kod + adat) */
    u32 bss_size;     /* nullazando terulet a kep utan */
    u32 stack_size;   /* keres, Phase 2 */
    u32 flags;
    u32 pad;
} PACKED;

/* A kernel altal a programnak atadott API (Phase 1, kernel mod). */
struct ao_api {
    u32 version;
    u32 pad;
    void (*puts)(const char *s);
    int  (*getc)(void);                 /* blokkol, ASCII/Latin-2 vagy KEY_* */
    u64  (*ticks)(void);                /* 10 ms */
    void *(*alloc)(usize n);
    void (*free)(void *p);
    int  (*printf)(const char *fmt, ...);
};

typedef int (*aox_entry_fn)(const struct ao_api *api, int argc, char **argv);
