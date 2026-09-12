/* Linearis framebuffer. A VBE-mod es a pitch a bootinfo-bol jon, a rajzolo sosem
 * a szelesseget hasznalja sorhossznak. Phase 0: kitoltes es par glyph. */
#include "fb.h"
#include "layout.h"

struct fb fb;

void fb_init(const struct bootinfo *bi)
{
    fb.base   = P2V(bi->fb_paddr);
    fb.width  = bi->fb_width;
    fb.height = bi->fb_height;
    fb.pitch  = bi->fb_pitch;
    fb.bpp    = bi->fb_bpp;
    fb.rpos   = bi->fb_rpos;
    fb.gpos   = bi->fb_gpos;
    fb.bpos   = bi->fb_bpos;
}

u32 fb_rgb(u8 r, u8 g, u8 b)
{
    return ((u32)r << fb.rpos) | ((u32)g << fb.gpos) | ((u32)b << fb.bpos);
}

static inline void put(u32 x, u32 y, u32 c)
{
    *(volatile u32 *)(fb.base + (u64)y * fb.pitch + (u64)x * 4) = c;
}

void fb_rect(u32 x, u32 y, u32 w, u32 h, u32 color)
{
    if (x >= fb.width || y >= fb.height)
        return;
    if (x + w > fb.width)
        w = fb.width - x;
    if (y + h > fb.height)
        h = fb.height - y;
    for (u32 j = 0; j < h; j++) {
        volatile u32 *row = (volatile u32 *)(fb.base + (u64)(y + j) * fb.pitch) + x;
        for (u32 i = 0; i < w; i++)
            row[i] = color;
    }
}

void fb_fill(u32 color)
{
    fb_rect(0, 0, fb.width, fb.height, color);
}

void fb_glyph(u32 x, u32 y, const u8 rows[16], u32 scale, u32 fg)
{
    for (u32 r = 0; r < 16; r++) {
        u8 bits = rows[r];
        for (u32 c = 0; c < 8; c++) {
            if (!(bits & (0x80 >> c)))
                continue;
            if (scale == 1)
                put(x + c, y + r, fg);
            else
                fb_rect(x + c * scale, y + r * scale, scale, scale, fg);
        }
    }
}
