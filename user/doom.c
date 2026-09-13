/* Doom az AO-OS-en. A motor a PureDOOM (user/doom/PureDOOM.h, id Software / Daivuk, GPL),
 * ez a fajl az AO-OS-hez kotes: fajlok a /state/games alol, memoria a sajat malloc-bol,
 * kep az fb capability-n at (320x200 haromszoros nagyitassal a kepernyo kozepen), billentyuk a
 * nyers konzolmodbol. Hang nincs.
 *   doom [-warp 1 1] [-skill 3] ...      (a /state/games/doom1.wad kell: fetch doom1.wad /state/games) */
#include "aolib.h"
#include "malloc.h"
#include "../kernel/lib/string.h"

#define DOOM_IMPLEMENTATION
#include "doom/PureDOOM.h"

#define WAD_DIR "/state/games"
#define SCALE 3

/* ---------------------------------------------------------------- fajlok */
struct dfile { int fd; int pos; int size; bool used; };
static struct dfile files[8];

static void *ao_doom_open(const char *filename, const char *mode)
{
    if (!mode || mode[0] == 'w' || mode[0] == 'a') return 0;      /* mentes, konfig-iras: most nincs */
    int fd = ao_open(filename, O_READ);
    if (fd < 0) return 0;
    for (int i = 0; i < 8; i++) {
        if (files[i].used) continue;
        struct stat st;
        files[i].fd = fd;
        files[i].pos = 0;
        files[i].size = ao_stat(filename, &st) == 0 ? (int)st.size : 0;
        files[i].used = true;
        return &files[i];
    }
    ao_close(fd);
    return 0;
}

static void ao_doom_close(void *h)
{
    struct dfile *f = h;
    if (!f || !f->used) return;
    ao_close(f->fd);
    f->used = false;
}

static int ao_doom_read(void *h, void *buf, int count)
{
    struct dfile *f = h;
    if (!f || count <= 0) return 0;
    int got = 0;
    while (got < count) {
        isize r = ao_read(f->fd, (u8 *)buf + got, (usize)(count - got));
        if (r <= 0) break;
        got += (int)r;
    }
    f->pos += got;
    return got;
}

static int ao_doom_write(void *h, const void *buf, int count) { (void)h; (void)buf; (void)count; return 0; }

static int ao_doom_seek(void *h, int offset, doom_seek_t origin)
{
    struct dfile *f = h;
    if (!f) return -1;
    int np = origin == DOOM_SEEK_SET ? offset : origin == DOOM_SEEK_CUR ? f->pos + offset : f->size + offset;
    if (np < 0) np = 0;
    ao_syscall(SYS_SEEK, (u64)f->fd, (u64)np, 0, 0);
    f->pos = np;
    return 0;
}

static int ao_doom_tell(void *h) { struct dfile *f = h; return f ? f->pos : 0; }
static int ao_doom_eof(void *h) { struct dfile *f = h; return f ? f->pos >= f->size : 1; }

/* ---------------------------------------------------------------- rendszer */
static void ao_doom_print(const char *s) { ao_puts(s); }
static void *ao_doom_malloc(int size) { return malloc((usize)(size > 0 ? size : 1)); }
static void ao_doom_free(void *p) { free(p); }
static void ao_doom_gettime(int *sec, int *usec)
{
    u64 t = ao_ticks();                         /* 10 ms */
    *sec = (int)(t / 100);
    *usec = (int)((t % 100) * 10000);
}
static void ao_doom_exit(int code) { ao_con_mode(CON_TEXT); ao_exit(code); }
static char *ao_doom_getenv(const char *var)
{
    if (!strcmp(var, "DOOMWADDIR") || !strcmp(var, "HOME")) return WAD_DIR;
    return 0;
}

/* ---------------------------------------------------------------- billentyuk */
static int map_key(u16 code)
{
    if (code >= 'a' && code <= 'z') return code;
    if (code >= '0' && code <= '9') return code;
    switch (code) {
    case '\n': return DOOM_KEY_ENTER;
    case '\t': return DOOM_KEY_TAB;
    case '\b': return DOOM_KEY_BACKSPACE;
    case ' ': return DOOM_KEY_SPACE;
    case '-': return DOOM_KEY_MINUS;
    case '=': case '+': return DOOM_KEY_EQUALS;
    case ',': return ',';
    case '.': return '.';
    case 0xE000: return DOOM_KEY_UP_ARROW;
    case 0xE001: return DOOM_KEY_DOWN_ARROW;
    case 0xE002: return DOOM_KEY_LEFT_ARROW;
    case 0xE003: return DOOM_KEY_RIGHT_ARROW;
    case 0xE00A: return DOOM_KEY_ESCAPE;
    case 0xE020: return DOOM_KEY_SHIFT;
    case 0xE021: return DOOM_KEY_CTRL;
    case 0xE022: case 0xE023: return DOOM_KEY_ALT;
    default: break;
    }
    if (code >= 0xE00B && code <= 0xE014) return DOOM_KEY_F1 + (code - 0xE00B);   /* F1..F10 */
    if (code >= 'A' && code <= 'Z') return code + 32;
    return -1;
}

/* ---------------------------------------------------------------- kep */
static struct fbinfo fi;
static u32 rowbuf[SCREENWIDTH * SCALE];

static void present(void)
{
    const unsigned char *src = doom_get_framebuffer(4);     /* RGBA, 320x200 */
    if (!src) return;
    u32 ox = (fi.width - SCREENWIDTH * SCALE) / 2, oy = (fi.height - SCREENHEIGHT * SCALE) / 2;
    u8 *base = (u8 *)(uptr)fi.vaddr;
    for (int y = 0; y < SCREENHEIGHT; y++) {
        const unsigned char *s = src + y * SCREENWIDTH * 4;
        for (int x = 0; x < SCREENWIDTH; x++) {
            u32 px = ((u32)s[x * 4] << fi.rpos) | ((u32)s[x * 4 + 1] << fi.gpos) | ((u32)s[x * 4 + 2] << fi.bpos);
            rowbuf[x * SCALE] = px; rowbuf[x * SCALE + 1] = px; rowbuf[x * SCALE + 2] = px;
        }
        for (int k = 0; k < SCALE; k++) {
            u32 *dst = (u32 *)(base + (u64)(oy + (u32)y * SCALE + (u32)k) * fi.pitch) + ox;
            memcpy(dst, rowbuf, sizeof rowbuf);
        }
    }
}

int main(int argc, char **argv)
{
    struct stat st;
    if (ao_stat(WAD_DIR "/doom1.wad", &st) != 0 && ao_stat(WAD_DIR "/doom.wad", &st) != 0 && ao_stat(WAD_DIR "/doom2.wad", &st) != 0) {
        ao_puts("doom: nincs WAD a /state/games alatt. PC-n: share/doom1.wad, netbookon: fetch doom1.wad /state/games\n");
        return 2;
    }
    int e = ao_fb_map(&fi);
    if (e) { ao_printf("doom: fb: %s (a manifestben kell az fb jog)\n", ao_errstr(e)); return 1; }
    if (fi.width < SCREENWIDTH * SCALE || fi.height < SCREENHEIGHT * SCALE) { ao_puts("doom: tul kicsi kepernyo\n"); return 1; }
    /* hatter: fekete */
    for (u32 y = 0; y < fi.height; y++) memset((u8 *)(uptr)fi.vaddr + (u64)y * fi.pitch, 0, (usize)fi.width * 4);

    doom_set_print(ao_doom_print);
    doom_set_malloc(ao_doom_malloc, ao_doom_free);
    doom_set_file_io(ao_doom_open, ao_doom_close, ao_doom_read, ao_doom_write, ao_doom_seek, ao_doom_tell, ao_doom_eof);
    doom_set_gettime(ao_doom_gettime);
    doom_set_exit(ao_doom_exit);
    doom_set_getenv(ao_doom_getenv);
    doom_set_default_int("key_up", DOOM_KEY_UP_ARROW);
    doom_set_default_int("key_down", DOOM_KEY_DOWN_ARROW);
    doom_set_default_int("key_left", DOOM_KEY_LEFT_ARROW);
    doom_set_default_int("key_right", DOOM_KEY_RIGHT_ARROW);
    doom_set_default_int("key_fire", DOOM_KEY_CTRL);
    doom_set_default_int("key_use", DOOM_KEY_SPACE);
    doom_set_default_int("key_strafe", DOOM_KEY_ALT);
    doom_set_default_int("key_speed", DOOM_KEY_SHIFT);
    doom_set_default_int("mouse_sensitivity", 0);

    ao_con_mode(CON_RAW | CON_NONBLOCK);
    doom_init(argc, argv, DOOM_FLAG_HIDE_MOUSE_OPTIONS | DOOM_FLAG_HIDE_SOUND_OPTIONS | DOOM_FLAG_HIDE_MUSIC_OPTIONS | DOOM_FLAG_MENU_DARKEN_BG);

    u64 last_tick = 0;
    for (;;) {
        struct key_ev ev[16];
        isize n = ao_read(0, ev, sizeof ev);
        for (isize i = 0; i < n / (isize)sizeof ev[0]; i++) {
            int k = map_key(ev[i].code);
            if (k < 0) continue;
            if (ev[i].down) doom_key_down((doom_key_t)k); else doom_key_up((doom_key_t)k);
        }
        doom_update();
        u64 t = ao_ticks() * 35 / 100;              /* Doom-tick (35 Hz) */
        if (t != last_tick) { present(); last_tick = t; }
        else ao_sleep(3);
    }
}
