#include "console.h"
#include "fb.h"
#include "font.h"
#include "serial.h"
#include "../mm/kheap.h"
#include "../lib/string.h"

#define SCROLLBACK_LINES 2000

struct cell {
    u8 ch;
    u8 attr;    /* fg | bg<<4 */
};

static struct cell *buf;        /* SCROLLBACK_LINES x cols gyuru */
static u32 cols, rows;
static u32 head;                /* a gyuru elso sora (legregebbi) */
static u32 nlines;              /* ervenyes sorok szama a gyuruben */
static u32 cur_col, cur_row;    /* kurzor a logikai (also) kepernyon: row 0..rows-1 */
static u32 view_off;            /* hany sorral gorgettunk vissza */
static u8  cur_attr;
static bool ready, mirror = true;
static u64 dirty_rows;          /* bit / kepernyo-sor (max 64) */
static bool all_dirty;
static u32 palette[16];
static u32 last_cursor_col = ~0u, last_cursor_row = ~0u;

/* ANSI-parser allapot */
static int esc_state;           /* 0 nincs, 1 ESC, 2 CSI */
static int esc_args[4], esc_nargs;

static void palette_init(void)
{
    static const u8 rgb[16][3] = {
        { 0x0C, 0x11, 0x16 }, { 0xE0, 0x50, 0x50 }, { 0x60, 0xC0, 0x80 }, { 0xE8, 0xB6, 0x4A },
        { 0x58, 0x90, 0xD0 }, { 0xC0, 0x70, 0xC0 }, { 0x60, 0xB8, 0xC8 }, { 0xC9, 0xD0, 0xD8 },
        { 0x7C, 0x8B, 0x99 }, { 0xFF, 0x80, 0x80 }, { 0x90, 0xF0, 0xB0 }, { 0xFF, 0xD8, 0x80 },
        { 0x90, 0xB8, 0xFF }, { 0xF0, 0xA0, 0xF0 }, { 0xA0, 0xE8, 0xF8 }, { 0xFF, 0xFF, 0xFF },
    };
    for (int i = 0; i < 16; i++)
        palette[i] = fb_rgb(rgb[i][0], rgb[i][1], rgb[i][2]);
}

static inline struct cell *line(u32 logical)   /* logical: 0 = legregebbi sor */
{
    return buf + ((head + logical) % SCROLLBACK_LINES) * cols;
}

/* a kepernyo r-edik sora (view_off figyelembe vetelevel) */
static inline struct cell *screen_line(u32 r)
{
    u32 first = nlines - rows - view_off;      /* nlines >= rows garantalt */
    return line(first + r);
}

static void clear_line(struct cell *l)
{
    for (u32 i = 0; i < cols; i++) {
        l[i].ch = ' ';
        l[i].attr = cur_attr;
    }
}

void console_init(void)
{
    cols = fb.width / FONT_W;
    rows = fb.height / FONT_H;
    if (rows > 64)
        rows = 64;
    buf = kmalloc(SCROLLBACK_LINES * cols * sizeof(struct cell));
    cur_attr = CON_DEFAULT_FG | (CON_DEFAULT_BG << 4);
    palette_init();
    head = 0;
    nlines = rows;
    for (u32 r = 0; r < rows; r++)
        clear_line(line(r));
    cur_col = cur_row = 0;
    view_off = 0;
    all_dirty = true;
    ready = true;
    fb_fill(palette[CON_DEFAULT_BG]);
}

bool console_ready(void) { return ready; }
u32 console_cols(void) { return cols; }
u32 console_rows(void) { return rows; }
void console_set_serial_mirror(bool on) { mirror = on; }
void console_get_cursor(u32 *c, u32 *r) { *c = cur_col; *r = cur_row; }

void console_set_col(u32 col)
{
    cur_col = col < cols ? col : cols - 1;
    dirty_rows |= 1ULL << cur_row;
}

void console_set_color(u8 fg, u8 bg)
{
    cur_attr = (fg & 15) | ((bg & 15) << 4);
}

static void mark_row(u32 r)
{
    dirty_rows |= 1ULL << r;
}

static void newline(void)
{
    cur_col = 0;
    if (cur_row + 1 < rows) {
        cur_row++;
        return;
    }
    /* uj sor a gyuru vegen */
    if (nlines < SCROLLBACK_LINES) {
        nlines++;
    } else {
        head = (head + 1) % SCROLLBACK_LINES;
    }
    clear_line(line(nlines - 1));
    all_dirty = true;
}

static void put_raw(u8 c)
{
    struct cell *l = line(nlines - rows + cur_row);
    l[cur_col].ch = c;
    l[cur_col].attr = cur_attr;
    mark_row(cur_row);
    if (++cur_col >= cols)
        newline();
}

static void apply_sgr(void)
{
    if (esc_nargs == 0) {
        esc_args[0] = 0;
        esc_nargs = 1;
    }
    for (int i = 0; i < esc_nargs; i++) {
        int a = esc_args[i];
        u8 fg = cur_attr & 15, bg = cur_attr >> 4;
        if (a == 0) { fg = CON_DEFAULT_FG; bg = CON_DEFAULT_BG; }
        else if (a == 1) { fg |= 8; }
        else if (a >= 30 && a <= 37) fg = (u8)(a - 30) | (fg & 8);
        else if (a >= 90 && a <= 97) fg = (u8)(a - 90 + 8);
        else if (a >= 40 && a <= 47) bg = (u8)(a - 40);
        else if (a >= 100 && a <= 107) bg = (u8)(a - 100 + 8);
        else if (a == 39) fg = CON_DEFAULT_FG;
        else if (a == 49) bg = CON_DEFAULT_BG;
        cur_attr = fg | (bg << 4);
    }
}

void console_putc(char ch)
{
    u8 c = (u8)ch;
    if (mirror)
        serial_putc(ch);
    if (view_off) {
        view_off = 0;
        all_dirty = true;
    }
    if (esc_state == 1) {
        if (c == '[') { esc_state = 2; esc_nargs = 0; esc_args[0] = 0; return; }
        esc_state = 0;
        return;
    }
    if (esc_state == 2) {
        if (c >= '0' && c <= '9') {
            esc_args[esc_nargs < 4 ? esc_nargs : 3] = esc_args[esc_nargs < 4 ? esc_nargs : 3] * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (esc_nargs < 3) esc_nargs++;
            esc_args[esc_nargs] = 0;
            return;
        }
        if (esc_nargs < 4) esc_nargs++;
        esc_state = 0;
        if (c == 'm') apply_sgr();
        else if (c == 'J') console_clear();
        else if (c == 'H') { cur_col = 0; cur_row = 0; }
        else if (c == 'K') {
            struct cell *l = line(nlines - rows + cur_row);
            for (u32 i = cur_col; i < cols; i++) { l[i].ch = ' '; l[i].attr = cur_attr; }
            mark_row(cur_row);
        }
        return;
    }
    switch (c) {
    case 0x1B: esc_state = 1; return;
    case '\n': mark_row(cur_row); newline(); return;
    case '\r': cur_col = 0; return;
    case '\b':
        if (cur_col) {
            cur_col--;
            struct cell *l = line(nlines - rows + cur_row);
            l[cur_col].ch = ' ';
            mark_row(cur_row);
        }
        return;
    case '\t':
        do { put_raw(' '); } while (cur_col & 7);
        return;
    default:
        if (c < 32) return;
        put_raw(c);
    }
}

void console_write(const char *s)
{
    while (*s)
        console_putc(*s++);
}

void console_clear(void)
{
    /* a lathato kepernyot uritjuk: rows uj ures sort tolunk a gyurube */
    for (u32 r = 0; r < rows; r++)
        newline();
    cur_col = 0;
    cur_row = 0;
    all_dirty = true;
}

void console_scroll_view(int lines)
{
    u32 max_off = nlines - rows;
    if (lines == 0) view_off = 0;
    else if (lines > 0) view_off = (view_off + (u32)lines > max_off) ? max_off : view_off + (u32)lines;
    else view_off = ((u32)(-lines) > view_off) ? 0 : view_off - (u32)(-lines);
    all_dirty = true;
}

/* egy kepernyo-sor kirajzolasa: 16 pixelsor x cols x 8 pixel, szekvencialis irasokkal */
static void draw_row(u32 r)
{
    const struct cell *l = screen_line(r);
    bool cursor_here = (view_off == 0 && r == cur_row);
    for (u32 py = 0; py < FONT_H; py++) {
        volatile u32 *px = (volatile u32 *)(fb.base + (u64)(r * FONT_H + py) * fb.pitch);
        for (u32 c = 0; c < cols; c++) {
            const struct cell *ce = &l[c];
            u32 fg = palette[ce->attr & 15], bg = palette[ce->attr >> 4];
            if (cursor_here && c == cur_col) { u32 t = fg; fg = bg; bg = t; if (fg == bg) fg = palette[CON_AMBER]; }
            u8 bits = font8x16[ce->ch][py];
            px[0] = (bits & 0x80) ? fg : bg;
            px[1] = (bits & 0x40) ? fg : bg;
            px[2] = (bits & 0x20) ? fg : bg;
            px[3] = (bits & 0x10) ? fg : bg;
            px[4] = (bits & 0x08) ? fg : bg;
            px[5] = (bits & 0x04) ? fg : bg;
            px[6] = (bits & 0x02) ? fg : bg;
            px[7] = (bits & 0x01) ? fg : bg;
            px += 8;
        }
    }
}

void console_flush(void)
{
    if (!ready)
        return;
    if (all_dirty) {
        for (u32 r = 0; r < rows; r++)
            draw_row(r);
        all_dirty = false;
        dirty_rows = 0;
    } else {
        u64 d = dirty_rows;
        if (last_cursor_row != cur_row && last_cursor_row < rows)
            d |= 1ULL << last_cursor_row;
        if (last_cursor_row != cur_row || last_cursor_col != cur_col)
            d |= 1ULL << cur_row;
        for (u32 r = 0; r < rows; r++)
            if (d & (1ULL << r))
                draw_row(r);
        dirty_rows = 0;
    }
    last_cursor_row = cur_row;
    last_cursor_col = cur_col;
}
