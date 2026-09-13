/* Egyszeru rajzolo konyvtar az fb capability-re: teglalap, RGB-kep, szoveg a rendszer 8x16-os
 * betukeszletevel (nagyithato). A kepernyot gfx_open() kepezi le, gfx_close() adja vissza a konzolnak. */
#pragma once
#include "aolib.h"

struct gfx {
    struct fbinfo fi;
    u8 *base;
    u32 w, h;
};

int  gfx_open(struct gfx *g);                    /* ao_fb_map; 0 vagy hibakod */
void gfx_close(struct gfx *g);                   /* ao_fb_release: a konzol visszarajzol */
u32  gfx_rgb(const struct gfx *g, u8 r, u8 gr, u8 b);
void gfx_fill(struct gfx *g, u32 x, u32 y, u32 w, u32 h, u32 px);
void gfx_frame(struct gfx *g, u32 x, u32 y, u32 w, u32 h, u32 thick, u32 px);   /* keret */
/* RGB (3 bajt/pixel) kep egy resze a kepernyore: a forras (sx, sy)-tol w x h, stride bajt/sor */
void gfx_blit_rgb(struct gfx *g, u32 dx, u32 dy, const u8 *rgb, u32 stride, u32 sx, u32 sy, u32 w, u32 h);
/* UTF-8 szoveg; scale = 1..4; bg 0xFFFFFFFF = atlatszo; visszaadja a kirajzolt szelesseget */
u32  gfx_text(struct gfx *g, u32 x, u32 y, u32 scale, u32 fg, u32 bg, const char *s);
u32  gfx_text_width(const char *s, u32 scale);
