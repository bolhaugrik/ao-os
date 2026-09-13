/* Framebuffer-konzol: cella-puffer + scrollback, dirty-soros rajzolas, soros tukor.
 * Nem VT100: \n \r \b \t, ESC[..m szin-reszhalmaz, ESC[2J, ESC[H. */
#pragma once
#include "types.h"

enum con_color {
    CON_BLACK, CON_RED, CON_GREEN, CON_YELLOW, CON_BLUE, CON_MAGENTA, CON_CYAN, CON_WHITE,
    CON_BBLACK, CON_BRED, CON_BGREEN, CON_BYELLOW, CON_BBLUE, CON_BMAGENTA, CON_BCYAN, CON_BWHITE,
};
#define CON_DEFAULT_FG CON_WHITE
#define CON_DEFAULT_BG CON_BLACK
#define CON_AMBER CON_YELLOW

void console_init(void);              /* fb_init utan, kheap utan */
bool console_ready(void);
void console_putc(char c);            /* pufferbe ir, nem rajzol */
void console_write(const char *s);
void console_flush(void);             /* piszkos sorok kirajzolasa */
void console_clear(void);
void console_set_color(u8 fg, u8 bg);
void console_scroll_view(int lines);  /* scrollback: + fel, - le, 0 = aljara */
u32  console_cols(void);
u32  console_rows(void);
void console_get_cursor(u32 *col, u32 *row);
void console_set_col(u32 col);        /* kurzor oszlopa az aktualis sorban */
void console_set_serial_mirror(bool on);
void console_suspend(bool on);        /* grafikus program fut: nem rajzol; visszakapcsolaskor mindent ujrarajzol */

/* Sorok visszaolvasasa (copy, shot). A sorszam monoton no a boot ota; a gyurubol mar
 * kiesett sor false-t ad. A szoveg UTF-8, zaro szokozok nelkul. */
u64  console_line_seq(void);                                  /* a kurzor soranak sorszama */
bool console_get_line(u64 seq, char *out, usize cap);
bool console_get_screen_line(u32 row, char *out, usize cap);  /* a lathato kepernyo r. sora */
