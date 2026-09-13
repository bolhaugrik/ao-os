/* i8042 PS/2 billentyuzet, scancode set 1 (a vezerlo fordit), IRQ1.
 * A key_event.code Unicode kodpont (ASCII, Latin-1, ő/ű = U+0151/U+0171),
 * a specialis billentyuk a maganhasznalatu tartomanyban (0xE000-tol). */
#pragma once
#include "types.h"

enum {
    KEY_NONE = 0,
    KEY_UP = 0xE000, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
    KEY_PGUP, KEY_PGDN, KEY_INS, KEY_DEL, KEY_ESC,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_SHIFT = 0xE020, KEY_CTRL, KEY_ALT, KEY_ALTGR, KEY_CAPS,     /* csak nyers modban */
};

#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4
#define MOD_ALTGR 8

struct key_event {
    u16 code;       /* Unicode kodpont vagy KEY_* (nyers modban a kiosztas modositatlan kodja) */
    u8  mods;
    u8  down;       /* 1 lenyomas, 0 felengedes (szoveges modban csak lenyomasok jonnek) */
    u64 tsc;        /* az IRQ idopontja */
};

#define KEY_IS_SPECIAL(c) ((c) >= 0xE000)
/* kodpont -> UTF-8 (max 3 bajt), visszaadja a hosszt */
static inline u32 utf8_encode(u16 cp, char out[4])
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

void kbd_init(void);
bool kbd_poll(struct key_event *ev);      /* nem blokkol */
void kbd_wait(struct key_event *ev);      /* blokkol, hlt-vel; soros bemenetet is figyel */
void kbd_tick(void);                      /* PIT-bol: soros bemenet figyelese, ebresztes */
void kbd_set_layout(const char *name);    /* "us" vagy "hu" */
/* nyers mod (jatekok): felengedes is esemeny, a modositok (Shift, Ctrl...) kulon billentyuk,
 * a kod a kiosztas modositatlan erteke (Shift/Caps/Ctrl nem alakitja at) */
void kbd_set_raw(bool on);
bool kbd_raw(void);
const char *kbd_layout(void);
