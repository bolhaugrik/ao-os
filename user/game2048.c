/* 2048 az AO-OS-en: ring 3-as program, teljes kepernyon, szines cellakkal.
 * Nyilak: csusztatas, r: uj jatek, q vagy Esc: kilepes. A legjobb eredmeny a /state/games/2048 fajlban. */
#include "aolib.h"
#include "../kernel/lib/string.h"
#include "../kernel/lib/fmt.h"

#define BEST_PATH "/state/games/2048"
#define CELL_W 9
#define CELL_H 3

static u32 g[4][4];
static u32 score, best;
static u64 rng;
static u32 W = 80, H = 25;
static bool won, over;
static char out[16384];
static usize on;

static u32 rnd(u32 n)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (u32)(rng % n);
}

static void emit(const char *s) { usize l = strlen(s); if (on + l < sizeof out) { memcpy(out + on, s, l); on += l; } }

static void spawn_tile(void)
{
    int empty[16][2], n = 0;
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) if (!g[r][c]) { empty[n][0] = r; empty[n][1] = c; n++; }
    if (!n) return;
    int k = (int)rnd((u32)n);
    g[empty[k][0]][empty[k][1]] = rnd(10) == 0 ? 4 : 2;
}

/* egy sor balra csusztatasa osszevonassal; true, ha valtozott */
static bool slide(u32 *v)
{
    u32 t[4] = { 0, 0, 0, 0 };
    int n = 0;
    bool moved = false;
    for (int i = 0; i < 4; i++) if (v[i]) t[n++] = v[i];
    for (int i = 0; i + 1 < n; i++) {
        if (t[i] == t[i + 1]) {
            t[i] *= 2;
            score += t[i];
            if (t[i] == 2048) won = true;
            for (int j = i + 1; j + 1 < n; j++) t[j] = t[j + 1];
            t[--n] = 0;
        }
    }
    for (int i = 0; i < 4; i++) { if (t[i] != v[i]) moved = true; v[i] = t[i]; }
    return moved;
}

static bool move(int dir)   /* 0 fel, 1 le, 2 bal, 3 jobb */
{
    bool moved = false;
    for (int k = 0; k < 4; k++) {
        u32 v[4];
        for (int i = 0; i < 4; i++) {
            int r = dir < 2 ? (dir == 0 ? i : 3 - i) : k;
            int c = dir < 2 ? k : (dir == 2 ? i : 3 - i);
            v[i] = g[r][c];
        }
        if (slide(v)) moved = true;
        for (int i = 0; i < 4; i++) {
            int r = dir < 2 ? (dir == 0 ? i : 3 - i) : k;
            int c = dir < 2 ? k : (dir == 2 ? i : 3 - i);
            g[r][c] = v[i];
        }
    }
    return moved;
}

static bool can_move(void)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            if (!g[r][c]) return true;
            if (c < 3 && g[r][c] == g[r][c + 1]) return true;
            if (r < 3 && g[r][c] == g[r + 1][c]) return true;
        }
    return false;
}

static void colors(u32 v, u32 *fg, u32 *bg)
{
    switch (v) {
    case 0: *fg = 90; *bg = 100; break;
    case 2: *fg = 30; *bg = 47; break;
    case 4: *fg = 30; *bg = 107; break;
    case 8: *fg = 30; *bg = 43; break;
    case 16: *fg = 30; *bg = 103; break;
    case 32: *fg = 97; *bg = 41; break;
    case 64: *fg = 97; *bg = 101; break;
    case 128: *fg = 97; *bg = 45; break;
    case 256: *fg = 30; *bg = 105; break;
    case 512: *fg = 97; *bg = 44; break;
    case 1024: *fg = 30; *bg = 104; break;
    case 2048: *fg = 30; *bg = 42; break;
    default: *fg = 30; *bg = 102; break;
    }
}

static void new_game(void)
{
    memset(g, 0, sizeof g);
    score = 0;
    won = over = false;
    spawn_tile();
    spawn_tile();
}

static void load_best(void)
{
    char buf[32];
    int fd = ao_open(BEST_PATH, O_READ);
    if (fd < 0) return;
    isize n = ao_read(fd, buf, sizeof buf - 1);
    ao_close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    best = 0;
    for (const char *p = buf; *p >= '0' && *p <= '9'; p++) best = best * 10 + (u32)(*p - '0');
}

static void save_best(void)
{
    if (score <= best) return;
    best = score;
    ao_mkdir("/state/games");
    int fd = ao_open(BEST_PATH, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) return;
    char buf[32];
    usize n = snformat(buf, sizeof buf, "%u\n", best);
    ao_write(fd, buf, n);
    ao_close(fd);
}

static void draw(bool full)
{
    on = 0;
    if (full) emit("\x1b[2J");
    u32 gw = 4 * CELL_W + 3, gh = 4 * (CELL_H + 1);
    u32 ox = W > gw ? (W - gw) / 2 : 0, oy = H > gh + 6 ? (H - gh - 6) / 2 + 3 : 3;
    char b[128];
    snformat(b, sizeof b, "\x1b[%u;%uH\x1b[0;93m2048\x1b[0m   pont: \x1b[97m%u\x1b[0m   legjobb: \x1b[97m%u\x1b[0m\x1b[K",
             oy - 2, ox + 1, score, best);
    emit(b);
    for (int r = 0; r < 4; r++) {
        for (int sub = 0; sub < CELL_H; sub++) {
            snformat(b, sizeof b, "\x1b[%u;%uH", oy + (u32)r * (CELL_H + 1) + (u32)sub, ox + 1);
            emit(b);
            for (int c = 0; c < 4; c++) {
                u32 fg, bg;
                colors(g[r][c], &fg, &bg);
                char num[16] = "";
                if (sub == CELL_H / 2 && g[r][c]) snformat(num, sizeof num, "%u", g[r][c]);
                u32 nl = (u32)strlen(num), left = (CELL_W - nl) / 2;
                char cell[CELL_W + 1];
                memset(cell, ' ', CELL_W);
                cell[CELL_W] = 0;
                memcpy(cell + left, num, nl);
                snformat(b, sizeof b, "\x1b[%u;%um%s\x1b[0m ", fg, bg, cell);
                emit(b);
            }
        }
        snformat(b, sizeof b, "\x1b[%u;%uH\x1b[K", oy + (u32)r * (CELL_H + 1) + CELL_H, ox + 1);
        emit(b);
    }
    snformat(b, sizeof b, "\x1b[%u;%uH\x1b[K", oy + gh, ox + 1);
    emit(b);
    if (over) emit("\x1b[91mNincs tobb lepes. r: uj jatek, q: kilep\x1b[0m");
    else if (won) emit("\x1b[92m2048! Mehet tovabb. \x1b[0m\x1b[90mnyilak, r: uj jatek, q: kilep\x1b[0m");
    else emit("\x1b[90mnyilak: csusztatas   r: uj jatek   q: kilep\x1b[0m");
    snformat(b, sizeof b, "\x1b[%u;%uH", H, W);
    emit(b);
    ao_write(1, out, on);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct sysinfo si;
    if (ao_sysinfo(&si) == 0 && si.con_cols >= 40 && si.con_rows >= 22) { W = si.con_cols; H = si.con_rows; }
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    rng = ((u64)hi << 32 | lo) ^ (ao_ticks() * 0x9E3779B97F4A7C15ULL) ^ 0xA5A5A5A5DEADBEEFULL;
    if (!rng) rng = 1;
    load_best();
    new_game();
    bool full = true;
    for (;;) {
        draw(full);
        full = false;
        char k[8];
        isize n = ao_read(0, k, sizeof k);
        if (n <= 0) break;
        int dir = -1;
        if (k[0] == 0x1b && n >= 3 && k[1] == '[') dir = k[2] == 'A' ? 0 : k[2] == 'B' ? 1 : k[2] == 'D' ? 2 : k[2] == 'C' ? 3 : -1;
        else if (k[0] == 0x1b || k[0] == 'q') break;
        else if (k[0] == 'r') { save_best(); new_game(); full = true; continue; }
        else if (k[0] == 'w' || k[0] == 'k') dir = 0;
        else if (k[0] == 's' || k[0] == 'j') dir = 1;
        else if (k[0] == 'a' || k[0] == 'h') dir = 2;
        else if (k[0] == 'd' || k[0] == 'l') dir = 3;
        if (dir < 0 || over) continue;
        if (move(dir)) {
            spawn_tile();
            if (!can_move()) { over = true; save_best(); }
        }
    }
    save_best();
    ao_puts("\x1b[2J\x1b[H");
    ao_printf("2048: pont %u, legjobb %u\n", score, best);
    return 0;
}
