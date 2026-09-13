/* fbtest: az fb capability probaja. Szinatmenetet es egy mozgo negyzetet rajzol kozvetlenul a
 * framebufferbe, egy billentyure kilep; a konzol utana visszarajzolja magat. */
#include "aolib.h"

static inline u32 rgb(const struct fbinfo *fi, u32 r, u32 g, u32 b)
{
    return (r << fi->rpos) | (g << fi->gpos) | (b << fi->bpos);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct fbinfo fi;
    int e = ao_fb_map(&fi);
    if (e) { ao_printf("fbtest: fb: %s\n", ao_errstr(e)); return 1; }
    u8 *base = (u8 *)(uptr)fi.vaddr;
    u64 t0 = ao_ticks();
    /* szinatmenet: 32 bites pixelek */
    for (u32 y = 0; y < fi.height; y++) {
        u32 *row = (u32 *)(base + (u64)y * fi.pitch);
        for (u32 x = 0; x < fi.width; x++)
            row[x] = rgb(&fi, x * 255 / fi.width, y * 255 / fi.height, 128);
    }
    u64 t1 = ao_ticks();
    /* mozgo negyzet, amig nem jon billentyu (nonblock nyers mod) */
    ao_con_mode(CON_RAW | CON_NONBLOCK);
    u32 sz = fi.width / 10, x = 0, dir = 1;
    for (int frame = 0; ; frame++) {
        for (u32 y = fi.height / 2 - sz / 2; y < fi.height / 2 + sz / 2; y++) {
            u32 *row = (u32 *)(base + (u64)y * fi.pitch);
            for (u32 i = 0; i < fi.width; i++)
                row[i] = (i >= x && i < x + sz) ? rgb(&fi, 255, 255, 255) : rgb(&fi, i * 255 / fi.width, y * 255 / fi.height, 128);
        }
        if (dir) { if (x + sz + 8 >= fi.width) dir = 0; else x += 8; } else { if (x < 8) dir = 1; else x -= 8; }
        struct key_ev ev;
        if (ao_read(0, &ev, sizeof ev) > 0 && ev.down) break;
        if (frame > 3000) break;
        ao_sleep(10);
    }
    ao_con_mode(CON_TEXT);
    ao_printf("fbtest: ok, %ux%u pitch %u, teljes kep %lu ms\n", fi.width, fi.height, fi.pitch, (t1 - t0) * 10);
    return 0;
}
