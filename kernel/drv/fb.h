#pragma once
#include "types.h"
#include "bootinfo.h"

struct fb {
    u8 *base;       /* virtualis cim (direkt map) */
    u32 width, height, pitch;
    u8 bpp, rpos, gpos, bpos;
};

extern struct fb fb;

void fb_init(const struct bootinfo *bi);
u32  fb_rgb(u8 r, u8 g, u8 b);
void fb_fill(u32 color);
void fb_rect(u32 x, u32 y, u32 w, u32 h, u32 color);
/* 8x16-os glyph rajzolasa scale-szeres nagyitassal */
void fb_glyph(u32 x, u32 y, const u8 rows[16], u32 scale, u32 fg);
