/* Capability-modell: nincs user, nincs rwx bit. Egy task egy csak-szukitheto
 * capability-keszletet birtokol; minden eroforrast megnevezo syscall a cap_check-en megy at. */
#pragma once
#include "types.h"

enum cap_kind {
    CAP_NONE = 0,
    CAP_FS_READ,     /* minta: utvonal-glob */
    CAP_FS_WRITE,
    CAP_EXEC,        /* minta: program-utvonal glob vagy nevesitett eszkoz */
    CAP_NET,         /* minta: host[:port] */
    CAP_TASK_SPAWN,  /* gyerek inditasa */
    CAP_SYS_INFO,
    CAP_SYS_POWER,   /* reboot / poweroff */
    CAP_CONSOLE,     /* konzol irasa/olvasasa */
    CAP_FB,          /* a framebuffer lekepezese a cimterbe (grafika, jatekok) */
    CAP_MAX_KIND
};

#define CAP_MAX 16
#define CAP_PATTERN_MAX 96

struct cap {
    u8 kind;
    char pattern[CAP_PATTERN_MAX];
};

struct capset {
    u32 n;
    struct cap caps[CAP_MAX];
    struct task_limits_ {
        u64 mem_bytes, cpu_ms, deadline_ms;
    } limits;
    char name[32];
};

struct task;

/* teljes jogu keszlet (gyoker-task) */
void capset_root(struct capset *cs);
/* manifest-szoveg feldolgozasa; false, ha hibas */
bool capset_parse(struct capset *cs, const char *text, usize len, char *err, usize errlen);
/* child ⊆ parent ? (minden child-cap illeszkedik egy parent-capre) */
bool capset_subset(const struct capset *child, const struct capset *parent);
/* ellenorzes + audit-naplo elutasitasnal */
bool cap_check(struct task *t, enum cap_kind kind, const char *resource);
/* glob: '*' egy komponensen belul, '**' tetszoleges melysegben */
bool cap_glob(const char *pattern, const char *path);
const char *cap_kind_name(enum cap_kind k);
int cap_kind_from_name(const char *s);

/* audit-gyuru */
struct audit_entry {
    u64 tick;
    u32 pid;
    u8  kind;
    u8  allowed;
    char resource[64];
};
u32 audit_count(void);
const struct audit_entry *audit_get(u32 i);   /* 0 = legregebbi a megorzottek kozul */
