#include "kbd.h"
#include "serial.h"
#include "../arch/io.h"
#include "../cpu/idt.h"
#include "../cpu/pic.h"
#include "../cpu/pit.h"
#include "../lib/string.h"

#define KBD_DATA 0x60
#define KBD_STAT 0x64
#define KBD_CMD  0x64

#define RING 64
static struct key_event ring[RING];
static volatile u32 rhead, rtail;

static u8 mods;
static bool caps, e0;
static int layout_id;   /* 0 us, 1 hu */

/* Latin-2 kodok a magyar betukhoz */
#define aa 0xE1
#define ee 0xE9
#define ii 0xED
#define oo 0xF3
#define ou 0xF6
#define od 0xF5
#define uu 0xFA
#define uy 0xFC
#define ud 0xFB
#define AA 0xC1
#define EE 0xC9
#define II 0xCD
#define OO 0xD3
#define OU 0xD6
#define OD 0xD5
#define UU 0xDA
#define UY 0xDC
#define UD 0xDB

struct keymap {
    u16 normal[128];
    u16 shift[128];
    u16 altgr[128];
};

static const struct keymap map_us = {
    .normal = {
        [0x01] = KEY_ESC, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
        'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0,
        KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
        0, 0, KEY_HOME, KEY_UP, KEY_PGUP, '-', KEY_LEFT, '5', KEY_RIGHT, '+', KEY_END, KEY_DOWN, KEY_PGDN, KEY_INS, KEY_DEL,
        0, 0, '<', KEY_F11, KEY_F12,
    },
    .shift = {
        [0x01] = KEY_ESC, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
        'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0,
        KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
        0, 0, KEY_HOME, KEY_UP, KEY_PGUP, '-', KEY_LEFT, '5', KEY_RIGHT, '+', KEY_END, KEY_DOWN, KEY_PGDN, KEY_INS, KEY_DEL,
        0, 0, '>', KEY_F11, KEY_F12,
    },
    .altgr = { 0 },
};

static const struct keymap map_hu = {
    .normal = {
        [0x01] = KEY_ESC, '1', '2', '3', '4', '5', '6', '7', '8', '9', ou, uy, oo, '\b', '\t',
        'q', 'w', 'e', 'r', 't', 'z', 'u', 'i', 'o', 'p', od, uu, '\n', 0,
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ee, aa, '0', 0, ud,
        'y', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' ', 0,
        KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
        0, 0, KEY_HOME, KEY_UP, KEY_PGUP, '-', KEY_LEFT, '5', KEY_RIGHT, '+', KEY_END, KEY_DOWN, KEY_PGDN, KEY_INS, KEY_DEL,
        0, 0, ii, KEY_F11, KEY_F12,
    },
    .shift = {
        [0x01] = KEY_ESC, '\'', '"', '+', '!', '%', '/', '=', '(', ')', OU, UY, OO, '\b', '\t',
        'Q', 'W', 'E', 'R', 'T', 'Z', 'U', 'I', 'O', 'P', OD, UU, '\n', 0,
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', EE, AA, 0xA7, 0, UD,
        'Y', 'X', 'C', 'V', 'B', 'N', 'M', '?', ':', '_', 0, '*', 0, ' ', 0,
        KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
        0, 0, KEY_HOME, KEY_UP, KEY_PGUP, '-', KEY_LEFT, '5', KEY_RIGHT, '+', KEY_END, KEY_DOWN, KEY_PGDN, KEY_INS, KEY_DEL,
        0, 0, II, KEY_F11, KEY_F12,
    },
    .altgr = {
        [0x02] = '~', 0, '^', 0, 0, 0, '`', 0, 0, 0, 0, 0, 0, 0,
        '\\', '|', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, '[', ']', 0, 0, 0, 0, '$', 0, 0, 0, 0,
        '>', '#', '&', '@', '{', '}', 0, ';', '>', '*', 0, 0, 0, 0, 0,
        [0x56] = '<',
    },
};

static const struct keymap *layouts[2] = { &map_us, &map_hu };

static void push(u16 code)
{
    u32 next = (rhead + 1) % RING;
    if (next == rtail)
        return;
    ring[rhead].code = code;
    ring[rhead].mods = mods;
    ring[rhead].tsc = rdtsc();
    rhead = next;
}

static void kbd_irq(struct regs *r)
{
    (void)r;
    u8 sc = inb(KBD_DATA);
    if (sc == 0xE0) { e0 = true; return; }
    if (sc == 0xE1) { return; }
    bool release = sc & 0x80;
    sc &= 0x7F;

    if (e0) {
        e0 = false;
        u16 code = 0;
        switch (sc) {
        case 0x1D: if (release) mods &= ~MOD_CTRL; else mods |= MOD_CTRL; return;
        case 0x38: if (release) mods &= ~MOD_ALTGR; else mods |= MOD_ALTGR; return;
        case 0x48: code = KEY_UP; break;
        case 0x50: code = KEY_DOWN; break;
        case 0x4B: code = KEY_LEFT; break;
        case 0x4D: code = KEY_RIGHT; break;
        case 0x47: code = KEY_HOME; break;
        case 0x4F: code = KEY_END; break;
        case 0x49: code = KEY_PGUP; break;
        case 0x51: code = KEY_PGDN; break;
        case 0x52: code = KEY_INS; break;
        case 0x53: code = KEY_DEL; break;
        case 0x1C: code = '\n'; break;
        case 0x35: code = '/'; break;
        default: return;
        }
        if (!release)
            push(code);
        return;
    }

    switch (sc) {
    case 0x2A: case 0x36: if (release) mods &= ~MOD_SHIFT; else mods |= MOD_SHIFT; return;
    case 0x1D: if (release) mods &= ~MOD_CTRL; else mods |= MOD_CTRL; return;
    case 0x38: if (release) mods &= ~MOD_ALT; else mods |= MOD_ALT; return;
    case 0x3A: if (!release) caps = !caps; return;
    }
    if (release)
        return;

    const struct keymap *km = layouts[layout_id];
    u16 code;
    if (mods & MOD_ALTGR)
        code = km->altgr[sc];
    else if (mods & MOD_SHIFT)
        code = km->shift[sc];
    else
        code = km->normal[sc];
    if (!code)
        return;
    if (caps && code < 0x100) {
        u8 c = (u8)code;
        bool lower = (c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE);
        bool upper = (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE);
        if (lower) code = c - 0x20;
        else if (upper) code = c + 0x20;
    }
    if ((mods & MOD_CTRL) && code < 0x100) {
        u8 c = (u8)code;
        if (c >= 'a' && c <= 'z') code = c - 'a' + 1;
        else if (c >= 'A' && c <= 'Z') code = c - 'A' + 1;
    }
    push(code);
}

static bool wait_write(void)
{
    for (u32 i = 0; i < 100000; i++)
        if (!(inb(KBD_STAT) & 2))
            return true;
    return false;
}

static bool wait_read(void)
{
    for (u32 i = 0; i < 100000; i++)
        if (inb(KBD_STAT) & 1)
            return true;
    return false;
}

static void flush_out(void)
{
    for (int i = 0; i < 64 && (inb(KBD_STAT) & 1); i++)
        inb(KBD_DATA);
}

void kbd_init(void)
{
    layout_id = 0;
    /* portok tiltasa, puffer urites */
    wait_write(); outb(KBD_CMD, 0xAD);
    wait_write(); outb(KBD_CMD, 0xA7);
    flush_out();
    /* konfiguracio: IRQ1 be, IRQ12 ki, forditas be, 2. port oraja tiltva */
    wait_write(); outb(KBD_CMD, 0x20);
    u8 cfg = wait_read() ? inb(KBD_DATA) : 0;
    cfg |= 0x01;     /* IRQ1 */
    cfg &= ~0x02;    /* IRQ12 */
    cfg |= 0x40;     /* translate set2 -> set1 */
    cfg |= 0x20;     /* 2. port oraja ki */
    cfg &= ~0x10;    /* 1. port oraja be */
    wait_write(); outb(KBD_CMD, 0x60);
    wait_write(); outb(KBD_DATA, cfg);
    /* 1. port engedelyezese, billentyuzet scan be */
    wait_write(); outb(KBD_CMD, 0xAE);
    wait_write(); outb(KBD_DATA, 0xF4);
    if (wait_read()) inb(KBD_DATA);   /* ACK */
    flush_out();

    irq_register(1, kbd_irq);
    pic_unmask(1);
}

bool kbd_poll(struct key_event *ev)
{
    if (rtail == rhead)
        return false;
    *ev = ring[rtail];
    rtail = (rtail + 1) % RING;
    return true;
}

/* soros bemenet: QEMU-tesztekhez. ESC [ A/B/C/D nyilak, \r Enter, 0x7F backspace */
static bool serial_key(struct key_event *ev)
{
    static int st;
    while (serial_has_input()) {
        u8 c = serial_getc();
        ev->mods = 0;
        ev->tsc = rdtsc();
        if (st == 0) {
            if (c == 0x1B) { st = 1; continue; }
            if (c == '\r') c = '\n';
            if (c == 0x7F) c = '\b';
            ev->code = c;
            return true;
        }
        if (st == 1) { st = (c == '[') ? 2 : 0; continue; }
        st = 0;
        switch (c) {
        case 'A': ev->code = KEY_UP; return true;
        case 'B': ev->code = KEY_DOWN; return true;
        case 'C': ev->code = KEY_RIGHT; return true;
        case 'D': ev->code = KEY_LEFT; return true;
        case 'H': ev->code = KEY_HOME; return true;
        case 'F': ev->code = KEY_END; return true;
        }
    }
    return false;
}

void kbd_wait(struct key_event *ev)
{
    for (;;) {
        if (kbd_poll(ev))
            return;
        if (serial_key(ev))
            return;
        idle_enter();
    }
}

void kbd_set_layout(const char *name)
{
    layout_id = (strcmp(name, "hu") == 0) ? 1 : 0;
}

const char *kbd_layout(void)
{
    return layout_id ? "hu" : "us";
}
