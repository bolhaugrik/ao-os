/* i8042 PS/2 billentyuzet, scancode set 1 (a vezerlo fordit), IRQ1. */
#pragma once
#include "types.h"

enum {
    KEY_NONE = 0,
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
    KEY_PGUP, KEY_PGDN, KEY_INS, KEY_DEL, KEY_ESC,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
};

#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4
#define MOD_ALTGR 8

struct key_event {
    u16 code;       /* ASCII / Latin-2 bajt, vagy KEY_* */
    u8  mods;
    u64 tsc;        /* az IRQ idopontja */
};

void kbd_init(void);
bool kbd_poll(struct key_event *ev);      /* nem blokkol */
void kbd_wait(struct key_event *ev);      /* blokkol, hlt-vel; soros bemenetet is figyel */
void kbd_tick(void);                      /* PIT-bol: soros bemenet figyelese, ebresztes */
void kbd_set_layout(const char *name);    /* "us" vagy "hu" */
const char *kbd_layout(void);
