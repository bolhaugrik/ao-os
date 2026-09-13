#include "gfx.h"
#include "../kernel/drv/font.h"
#include "../kernel/lib/string.h"

int gfx_open(struct gfx *g)
{
    int e = ao_fb_map(&g->fi);
    if (e) return e;
    g->base = (u8 *)(uptr)g->fi.vaddr;
    g->w = g->fi.width;
    g->h = g->fi.height;
    return 0;
}

void gfx_close(struct gfx *g) { (void)g; ao_fb_release(); }

u32 gfx_rgb(const struct gfx *g, u8 r, u8 gr, u8 b)
{
    return ((u32)r << g->fi.rpos) | ((u32)gr << g->fi.gpos) | ((u32)b << g->fi.bpos);
}

static inline u32 *row(struct gfx *g, u32 y) { return (u32 *)(g->base + (u64)y * g->fi.pitch); }

void gfx_fill(struct gfx *g, u32 x, u32 y, u32 w, u32 h, u32 px)
{
    if (x >= g->w || y >= g->h) return;
    if (x + w > g->w) w = g->w - x;
    if (y + h > g->h) h = g->h - y;
    for (u32 j = 0; j < h; j++) {
        u32 *r = row(g, y + j) + x;
        for (u32 i = 0; i < w; i++) r[i] = px;
    }
}

void gfx_frame(struct gfx *g, u32 x, u32 y, u32 w, u32 h, u32 thick, u32 px)
{
    gfx_fill(g, x, y, w, thick, px);
    gfx_fill(g, x, y + h > thick ? y + h - thick : y, w, thick, px);
    gfx_fill(g, x, y, thick, h, px);
    gfx_fill(g, x + w > thick ? x + w - thick : x, y, thick, h, px);
}

void gfx_blit_rgb(struct gfx *g, u32 dx, u32 dy, const u8 *rgb, u32 stride, u32 sx, u32 sy, u32 w, u32 h)
{
    if (dx >= g->w || dy >= g->h) return;
    if (dx + w > g->w) w = g->w - dx;
    if (dy + h > g->h) h = g->h - dy;
    for (u32 j = 0; j < h; j++) {
        const u8 *s = rgb + (u64)(sy + j) * stride + (u64)sx * 3;
        u32 *r = row(g, dy + j) + dx;
        for (u32 i = 0; i < w; i++, s += 3)
            r[i] = ((u32)s[0] << g->fi.rpos) | ((u32)s[1] << g->fi.gpos) | ((u32)s[2] << g->fi.bpos);
    }
}

/* Unicode kodpont -> glyph-index (Latin-2 elrendezes, mint a konzolban) */
static u8 glyph_for(u32 cp)
{
    if (cp < 0x80) return (u8)cp;
    switch (cp) {
    case 0xA7: case 0xC1: case 0xC9: case 0xCD: case 0xD3: case 0xD6: case 0xDA: case 0xDC:
    case 0xE1: case 0xE9: case 0xED: case 0xF3: case 0xF6: case 0xFA: case 0xFC:
        return (u8)cp;
    case 0x150: return 0xD5;
    case 0x151: return 0xF5;
    case 0x170: return 0xDB;
    case 0x171: return 0xFB;
    case 0x2013: case 0x2014: return '-';
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: case 0x201E: return '"';
    case 0x2026: return '.';
    case 0xA0: return ' ';
    default: return 0x7F;
    }
}

static const char *next_cp(const char *s, u32 *cp)
{
    u8 c = (u8)*s;
    if (c < 0x80) { *cp = c; return s + 1; }
    int need = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
    u32 v = c & (0x3F >> need);
    s++;
    for (int i = 0; i < need && ((u8)*s & 0xC0) == 0x80; i++) v = (v << 6) | ((u8)*s++ & 0x3F);
    *cp = v;
    return s;
}

u32 gfx_text_width(const char *s, u32 scale)
{
    u32 n = 0, cp;
    while (*s) { s = next_cp(s, &cp); n++; }
    return n * FONT_W * scale;
}

u32 gfx_text(struct gfx *g, u32 x, u32 y, u32 scale, u32 fg, u32 bg, const char *s)
{
    if (scale < 1) scale = 1;
    u32 x0 = x, cp;
    while (*s) {
        s = next_cp(s, &cp);
        if (x + FONT_W * scale > g->w) break;
        const u8 *rows = font8x16[glyph_for(cp)];
        for (u32 gy = 0; gy < FONT_H; gy++) {
            u8 bits = rows[gy];
            for (u32 sy = 0; sy < scale; sy++) {
                u32 py = y + gy * scale + sy;
                if (py >= g->h) break;
                u32 *r = row(g, py) + x;
                for (u32 gx = 0; gx < FONT_W; gx++) {
                    bool on = bits & (0x80 >> gx);
                    if (!on && bg == 0xFFFFFFFFu) continue;
                    u32 px = on ? fg : bg;
                    for (u32 sx = 0; sx < scale; sx++) r[gx * scale + sx] = px;
                }
            }
        }
        x += FONT_W * scale;
    }
    return x - x0;
}
