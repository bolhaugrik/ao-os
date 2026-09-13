/* projector: egy weboldal vagy kereses szemantikus lenyomata a teljes kepernyon.
 * A hid (tools/projector.py) tolti le es csupaszitja le az oldalt, a netbook csak a lenyomatot
 * (JSON, docs/AOP.md) kapja a titkositott AOP-csatornan, es itt rendezi ujra: bal oszlop vazlat,
 * kozep tartalom, jobb oszlop linkek. Szin = csomoponttipus es suly, inverz = fokusz.
 *
 *   projector CIM | KERDES         teljes kepernyo
 *   projector --dump CIM | KERDES  a lenyomat szovegkent (tesztekhez, agenteknek)
 * Billentyuk: nyilak, PgUp/PgDn, Tab oszlopvaltas, Enter link/vazlat, Backspace vissza,
 * / kereses, n kovetkezo talalat, s mentes (/state/projector/), q vagy Esc kilepes. */
#include "aop.h"
#include "json.h"
#include "gfx.h"
#include "inflate.h"
#include "malloc.h"
#include "../kernel/lib/string.h"
#include "../kernel/lib/fmt.h"

#define MAX_NODES 512
#define MAX_LINES 4096
#define MAX_SRC 32
#define ARENA_SIZE 60000
#define HIST_MAX 16
#define MAX_ROWS 64
#define ROW_BYTES 1024

enum { N_TITLE, N_HEAD, N_PARA, N_ITEM, N_QUOTE, N_CODE, N_LINK, N_ROW, N_NOTE, N_FACT, N_TYPES };
static const char *const type_names[N_TYPES] = { "title", "head", "para", "item", "quote", "code", "link", "row", "note", "fact" };
static const u8 type_color[N_TYPES] = { 93, 33, 37, 36, 35, 32, 94, 90, 90, 92 };

struct node { u8 t, w; u16 s; u32 x, to; };
static struct node nodes[MAX_NODES];
static int nnodes;
static char arena[ARENA_SIZE];
static usize apos = 1;                              /* arena[0] = "" */
static char title[256], query[512];
static int nsrcs;
static u8 raw[AOP_PAYLOAD_MAX + 1];                 /* a nyers JSON a menteshez */
static usize rawlen;

/* nezet */
static u32 W, H, LW, RW, CW, CR;                    /* meretek: bal, jobb, kozep szelesseg; tartalom-sorok */
static u16 content[MAX_NODES], links[MAX_NODES], outline[MAX_NODES];
static int ncontent, nlinks, noutline;
struct line { u16 node; u16 off; u16 len; u8 pre; };   /* pre: elotag-szelesseg (folytato sornal behuzas) */
static struct line lines[MAX_LINES];
static int nlines;
static int node_first_line[MAX_NODES];
static int col_focus;                               /* 0 vazlat, 1 tartalom, 2 linkek */
static int cfocus, lfocus, ofocus, scroll_line, oscroll, lscroll;
static char message[160];
static char search[96];
static bool searching;
static char hist[HIST_MAX][512];
static int nhist;
static char cur_url[512];                           /* az aktualis oldal (mindket nezet) */
static bool view_valid, text_stale;                 /* a hu kep / a lenyomat a cur_url-hoz tartozik-e */

/* ---------------------------------------------------------------- segedek */
static u32 intern(const char *s, usize n)
{
    if (apos + n + 1 > ARENA_SIZE) { if (apos + 1 >= ARENA_SIZE) return 0; n = ARENA_SIZE - apos - 1; }
    memcpy(arena + apos, s, n);
    arena[apos + n] = 0;
    u32 off = (u32)apos;
    apos += n + 1;
    return off;
}

static const char *ntext(int i) { return arena + nodes[i].x; }
static const char *nlink(int i) { return arena + nodes[i].to; }
static bool is_cont(char c) { return ((u8)c & 0xC0) == 0x80; }
static u32 width_of(const char *s, usize n) { u32 w = 0; for (usize i = 0; i < n; i++) if (!is_cont(s[i])) w++; return w; }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static bool contains_ci(const char *hay, const char *needle)
{
    usize nl = strlen(needle);
    if (!nl) return false;
    for (usize i = 0; hay[i]; i++) {
        usize k = 0;
        while (k < nl && hay[i + k] && lower(hay[i + k]) == lower(needle[k])) k++;
        if (k == nl) return true;
    }
    return false;
}

/* ---------------------------------------------------------------- lenyomat beolvasasa */
static bool parse_node(struct jp *j)
{
    static char tmp[8192];
    if (!jp_expect(j, '{')) return false;
    struct node nd = { N_PARA, 5, 0, 0, 0 };
    if (!jp_expect(j, '}')) {
        for (;;) {
            char key[16];
            if (!jp_key(j, key, sizeof key)) return false;
            if (!strcmp(key, "t")) {
                if (!jp_string(j, tmp, 32, NULL)) return false;
                for (int k = 0; k < N_TYPES; k++) if (!strcmp(tmp, type_names[k])) nd.t = (u8)k;
            } else if (!strcmp(key, "w")) {
                i64 v; if (!jp_number(j, &v)) return false; nd.w = (u8)(v < 0 ? 0 : v > 9 ? 9 : v);
            } else if (!strcmp(key, "s")) {
                i64 v; if (!jp_number(j, &v)) return false; nd.s = (u16)v;
            } else if (!strcmp(key, "x")) {
                usize l; if (!jp_string(j, tmp, sizeof tmp, &l)) return false; nd.x = intern(tmp, l);
            } else if (!strcmp(key, "to")) {
                usize l; if (!jp_string(j, tmp, sizeof tmp, &l)) return false; nd.to = intern(tmp, l);
            } else if (!jp_skip(j)) {
                return false;
            }
            if (jp_expect(j, ',')) continue;
            if (!jp_expect(j, '}')) return false;
            break;
        }
    }
    if (nnodes < MAX_NODES && arena[nd.x]) nodes[nnodes++] = nd;
    return true;
}

static bool parse_array(struct jp *j, bool is_nodes)
{
    if (!jp_expect(j, '[')) return false;
    if (jp_expect(j, ']')) return true;
    for (;;) {
        if (is_nodes) { if (!parse_node(j)) return false; }
        else { if (!jp_skip(j)) return false; nsrcs++; }
        if (jp_expect(j, ',')) continue;
        return jp_expect(j, ']');
    }
}

static bool parse_imprint(const char *text, usize len)
{
    nnodes = 0; nsrcs = 0; apos = 1; title[0] = 0;
    struct jp j;
    jp_init(&j, text, len);
    if (!jp_expect(&j, '{')) return false;
    if (jp_expect(&j, '}')) return true;
    for (;;) {
        char key[24];
        if (!jp_key(&j, key, sizeof key)) return false;
        bool ok;
        if (!strcmp(key, "title")) ok = jp_string(&j, title, sizeof title, NULL);
        else if (!strcmp(key, "q")) ok = jp_string(&j, query, sizeof query, NULL);
        else if (!strcmp(key, "nodes")) ok = parse_array(&j, true);
        else if (!strcmp(key, "sources")) ok = parse_array(&j, false);
        else ok = jp_skip(&j);
        if (!ok) return false;
        if (jp_expect(&j, ',')) continue;
        return jp_expect(&j, '}');
    }
}

/* ---------------------------------------------------------------- betoltes a hidrol */
static bool load(const char *q)
{
    char req[600];
    usize n = 0;
    memcpy(req, "q=", 2); n = 2;
    usize ql = strlen(q);
    if (ql > 500) ql = 500;
    memcpy(req + n, q, ql); n += ql;
    memcpy(req + n, "\ndepth=0\n", 9); n += 9;
    int e = aop_send(AOP_PROJECT, req, n);
    if (e) { ao_printf("projector: kuldes: %s\n", ao_errstr(e)); return false; }
    for (;;) {
        u16 type;
        usize len;
        e = aop_recv(&type, &len);
        if (e) { ao_printf("projector: kapcsolat: %s\n", ao_errstr(e)); return false; }
        if (type == AOP_IMPRINT) {
            memcpy(raw, aop_payload, len);
            raw[len] = 0;
            rawlen = len;
            if (!parse_imprint((const char *)aop_payload, len)) { ao_puts("projector: hibas lenyomat (JSON)\n"); return false; }
            if (!title[0]) strlcpy(title, q, sizeof title);
            return true;
        }
        if (type == AOP_ERR) { ao_printf("[hid hiba: %s]\n", (char *)aop_payload); return false; }
        if (type == AOP_END) { ao_puts("projector: nem jott lenyomat\n"); return false; }
        /* DELTA (haladas) es mas: figyelmen kivul */
    }
}

/* ---------------------------------------------------------------- elrendezes */
static u8 prefix_width(int t) { return t == N_ITEM || t == N_QUOTE ? 2 : t == N_LINK ? 3 : 0; }

static void layout(void)
{
    LW = W >= 100 ? 26 : 0;
    RW = W >= 140 ? 36 : 0;
    CW = W - LW - RW - (LW ? 1 : 0) - (RW ? 1 : 0);
    CR = H - 2;
    ncontent = nlinks = noutline = 0;
    for (int i = 0; i < nnodes; i++) {
        if (nodes[i].t == N_LINK && RW) links[nlinks++] = (u16)i;
        else content[ncontent++] = (u16)i;
        if (nodes[i].t == N_TITLE || nodes[i].t == N_HEAD) outline[noutline++] = (u16)i;
    }
    /* sorokra tordeles a kozep szelessegere */
    nlines = 0;
    int prev_t = -1;
    for (int ci = 0; ci < ncontent && nlines < MAX_LINES - 2; ci++) {
        int i = content[ci];
        int t = nodes[i].t;
        if (nlines && !((t == N_ITEM || t == N_ROW) && prev_t == t))
            lines[nlines++] = (struct line){ 0xFFFF, 0, 0, 0 };
        prev_t = t;
        node_first_line[ci] = nlines;
        const char *s = ntext(i);
        usize len = strlen(s);
        u8 pw = prefix_width(t);
        u32 avail = CW > pw + 4 ? CW - pw : 4;
        usize pos = 0;
        while (pos < len && nlines < MAX_LINES - 1) {
            /* egy sor: legfeljebb avail oszlop, szohataron torve */
            usize end = pos, last_space = 0;
            u32 w = 0;
            while (end < len && s[end] != '\n') {
                if (!is_cont(s[end])) {
                    if (w == avail) break;
                    w++;
                }
                if (s[end] == ' ') last_space = end;
                end++;
            }
            usize cut = end;
            if (end < len && s[end] != '\n' && last_space > pos) cut = last_space;
            if (t == N_CODE) cut = end;                 /* kod: nem tordelunk szonal, csonkolunk */
            lines[nlines++] = (struct line){ (u16)ci, (u16)pos, (u16)(cut - pos), pw };
            if (t == N_CODE) {
                while (end < len && s[end] != '\n') end++;      /* a sor maradeka levagva */
                pos = end < len ? end + 1 : end;                /* a kovetkezo sor behuzasa megmarad */
                continue;
            }
            pos = cut;
            while (pos < len && (s[pos] == ' ' || s[pos] == '\n')) pos++;
        }
        if (pos == 0) lines[nlines++] = (struct line){ (u16)ci, 0, 0, pw };
    }
    if (cfocus >= ncontent) cfocus = ncontent ? ncontent - 1 : 0;
    if (lfocus >= nlinks) lfocus = nlinks ? nlinks - 1 : 0;
    if (ofocus >= noutline) ofocus = noutline ? noutline - 1 : 0;
}

/* ---------------------------------------------------------------- rajzolas */
static char out[120000];
static usize on;
static char prev_rows[MAX_ROWS][ROW_BYTES];
static u16 prev_len[MAX_ROWS];
static bool full_redraw;
static char row[ROW_BYTES];
static usize rn;
static u32 rcol;

static void emit(const char *s, usize n) { if (on + n < sizeof out) { memcpy(out + on, s, n); on += n; } }
static void emit_str(const char *s) { emit(s, strlen(s)); }

static void rb_reset(void) { rn = 0; rcol = 0; }
static void rb_raw(const char *s) { usize l = strlen(s); if (rn + l < ROW_BYTES) { memcpy(row + rn, s, l); rn += l; } }
static void rb_color(u8 fg, u8 bg)
{
    char b[24];
    snformat(b, sizeof b, "\x1b[0;%u;%um", (u32)fg, (u32)bg);
    rb_raw(b);
}
/* szoveg legfeljebb limit oszlopig (a sor abszolut oszlopa) */
static void rb_text(const char *s, usize n, u32 limit)
{
    for (usize i = 0; i < n; i++) {
        if (!is_cont(s[i])) { if (rcol >= limit) return; rcol++; }
        if (s[i] == '\n' || s[i] == '\t' || (u8)s[i] < 32) { if (rn + 1 < ROW_BYTES) row[rn++] = ' '; continue; }
        if (rn + 1 < ROW_BYTES) row[rn++] = s[i];
    }
}
static void rb_pad(u32 upto) { while (rcol < upto && rn + 1 < ROW_BYTES) { row[rn++] = ' '; rcol++; } }

static void rb_flush(u32 r)
{
    rb_raw("\x1b[0m");
    if (!full_redraw && prev_len[r] == rn && memcmp(prev_rows[r], row, rn) == 0) return;
    char pos[16];
    snformat(pos, sizeof pos, "\x1b[%u;1H", r + 1);
    emit_str(pos);
    emit(row, rn);
    memcpy(prev_rows[r], row, rn);
    prev_len[r] = (u16)rn;
}

static void draw_content_line(int li, u32 x0)
{
    if (li >= nlines) { rb_pad(x0 + CW); return; }
    struct line *l = &lines[li];
    if (l->node == 0xFFFF) { rb_pad(x0 + CW); return; }
    int i = content[l->node];
    struct node *nd = &nodes[i];
    bool foc = col_focus == 1 && l->node == cfocus;
    u8 fg = nd->w < 3 ? 90 : type_color[nd->t];
    if (foc) rb_color(30, 47); else rb_color(fg, 40);
    bool first = node_first_line[l->node] == li;
    if (nd->t == N_ITEM) rb_text(first ? "- " : "  ", 2, x0 + CW);
    else if (nd->t == N_QUOTE) rb_text("| ", 2, x0 + CW);
    else if (nd->t == N_LINK) rb_text(first ? "-> " : "   ", 3, x0 + CW);
    rb_text(ntext(i) + l->off, l->len, x0 + CW);
    rb_pad(x0 + CW);
}

static void draw(void)
{
    on = 0;
    if (full_redraw) emit_str("\x1b[2J");
    /* fejlec */
    rb_reset();
    rb_color(97, 44);
    rb_text(" PROJECTOR  ", 12, W);
    rb_text(title, strlen(title), W > 40 ? W - 28 : W);
    char info[64];
    snformat(info, sizeof info, "  %d csomopont, %d link, %d forras ", nnodes, nlinks, nsrcs);
    u32 iw = width_of(info, strlen(info));
    rb_pad(W > iw ? W - iw : rcol);
    rb_text(info, strlen(info), W);
    rb_pad(W);
    rb_flush(0);
    /* tartalom-sorok */
    for (u32 r = 0; r < CR; r++) {
        rb_reset();
        u32 x = 0;
        if (LW) {
            int oi = oscroll + (int)r;
            if (oi < noutline) {
                int i = outline[oi];
                bool foc = col_focus == 0 && oi == ofocus;
                if (foc) rb_color(30, 47); else rb_color(nodes[i].t == N_TITLE ? 93 : 33, 40);
                u32 ind = nodes[i].t == N_HEAD && nodes[i].w < 8 ? 8 - nodes[i].w : 0;
                if (ind > 4) ind = 4;
                rb_pad(ind);
                rb_text(ntext(i), strlen(ntext(i)), LW - 1);
            } else {
                rb_color(37, 40);
            }
            rb_pad(LW);
            rb_color(90, 40);
            rb_text("|", 1, LW + 1);
            x = LW + 1;
        }
        draw_content_line(scroll_line + (int)r, x);
        x += CW;
        if (RW) {
            rb_color(90, 40);
            rb_text("|", 1, x + 1);
            x++;
            int li = lscroll + (int)r;
            if (li < nlinks) {
                int i = links[li];
                bool foc = col_focus == 2 && li == lfocus;
                if (foc) rb_color(30, 47); else rb_color(94, 40);
                rb_text(" ", 1, W);
                rb_text(ntext(i), strlen(ntext(i)), W);
            } else {
                rb_color(37, 40);
            }
            rb_pad(W);
        }
        rb_flush(r + 1);
    }
    /* allapotsor */
    rb_reset();
    if (searching) {
        rb_color(30, 43);
        rb_text(" / ", 3, W);
        rb_text(search, strlen(search), W);
        rb_text("_", 1, W);
    } else if (message[0]) {
        rb_color(30, 42);
        rb_text(" ", 1, W);
        rb_text(message, strlen(message), W);
    } else {
        static const char help[] = " nyilak  Tab: oszlop  Enter: link/vazlat  Bksp: vissza  PgUp/PgDn  /: kereses  n: kovetkezo  s: mentes  v: hu kep  q: kilep";
        rb_color(90, 40);
        rb_text(help, sizeof help - 1, W);
    }
    rb_pad(W);
    rb_flush(H - 1);
    char pos[16];
    snformat(pos, sizeof pos, "\x1b[%u;%uH", H, W);
    emit_str(pos);
    ao_write(1, out, on);
    full_redraw = false;
}

/* ---------------------------------------------------------------- navigacio */
static void ensure_visible(void)
{
    if (!ncontent) return;
    int first = node_first_line[cfocus];
    int last = cfocus + 1 < ncontent ? node_first_line[cfocus + 1] - 1 : nlines - 1;
    if (last - first + 1 > (int)CR) last = first + (int)CR - 1;
    if (first < scroll_line) scroll_line = first;
    if (last >= scroll_line + (int)CR) scroll_line = last - (int)CR + 1;
    if (scroll_line < 0) scroll_line = 0;
    if (ofocus < oscroll) oscroll = ofocus;
    if (ofocus >= oscroll + (int)CR) oscroll = ofocus - (int)CR + 1;
    if (lfocus < lscroll) lscroll = lfocus;
    if (lfocus >= lscroll + (int)CR) lscroll = lfocus - (int)CR + 1;
}

/* a scroll_line-hoz tartozo elso csomopont */
static int node_at_line(int li)
{
    for (int c = 0; c < ncontent; c++)
        if (node_first_line[c] >= li) return c;
    return ncontent ? ncontent - 1 : 0;
}

static void find_next(void)
{
    if (!search[0] || !ncontent) return;
    for (int k = 1; k <= ncontent; k++) {
        int c = (cfocus + k) % ncontent;
        if (contains_ci(ntext(content[c]), search)) { cfocus = c; col_focus = 1; ensure_visible(); return; }
    }
    snformat(message, sizeof message, "nincs talalat: %s", search);
}

static void save(void)
{
    ao_mkdir("/state/projector");
    char path[128];
    usize n = 0;
    memcpy(path, "/state/projector/", 17); n = 17;
    bool dash = true;
    for (usize i = 0; title[i] && n < 17 + 32; i++) {
        char c = lower(title[i]);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { path[n++] = c; dash = false; }
        else if (!dash && !is_cont(c)) { path[n++] = '-'; dash = true; }
    }
    while (n > 17 && path[n - 1] == '-') n--;
    if (n == 17) { memcpy(path + n, "lenyomat", 8); n += 8; }
    memcpy(path + n, ".json", 6);
    int fd = ao_open(path, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) { snformat(message, sizeof message, "mentes: %s: %s", path, ao_errstr(fd)); return; }
    ao_write(fd, raw, rawlen);
    ao_close(fd);
    snformat(message, sizeof message, "mentve: %s (%lu bajt)", path, (u64)rawlen);
}

/* egy billentyu: kodpont vagy KEY_* (0xE000+), -1 hiba; a nyers bajtok key_raw-ban */
enum { K_UP = 0xE000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_ESC };
static char key_raw[8];
static usize key_rawn;

static int read_key(void)
{
    isize n = ao_read(0, key_raw, sizeof key_raw);
    if (n <= 0) return -1;
    key_rawn = (usize)n;
    u8 *b = (u8 *)key_raw;
    if (b[0] == 0x1b) {
        if (n == 1) return K_ESC;
        if (n >= 3 && b[1] == '[') {
            switch (b[2]) {
            case 'A': return K_UP;
            case 'B': return K_DOWN;
            case 'C': return K_RIGHT;
            case 'D': return K_LEFT;
            case 'H': return K_HOME;
            case 'F': return K_END;
            case '5': return K_PGUP;
            case '6': return K_PGDN;
            case '3': return K_DEL;
            }
        }
        return K_ESC;
    }
    if (b[0] < 0x80) return b[0];
    u32 cp = 0;
    int need = (b[0] & 0xE0) == 0xC0 ? 1 : (b[0] & 0xF0) == 0xE0 ? 2 : 3;
    cp = b[0] & (0x3F >> need);
    for (int i = 1; i <= need && i < n; i++) cp = (cp << 6) | (b[i] & 0x3F);
    return (int)cp;
}

static bool open_query(const char *q)
{
    ao_printf("projector: %s betoltese...\n", q);
    if (!load(q)) return false;
    strlcpy(cur_url, q, sizeof cur_url);
    view_valid = false;
    text_stale = false;
    cfocus = lfocus = ofocus = scroll_line = oscroll = lscroll = 0;
    col_focus = 1;
    layout();
    full_redraw = true;
    return true;
}

/* ---------------------------------------------------------------- hu kep nezet (RENDER a hidon) */
/* A PC bongeszomotorja rendereli az oldalt, a netbook a kepet mutatja az fb-n; a linkek helye
 * jon a keppel, igy Tab-bal lehet linkrol linkre lepni, Enter megnyitja. 'v' valt a ket nezet kozt. */
struct vlink { u32 x, y, w, h; u32 url; };
#define MAX_VLINKS 400
#define BAR 20
static struct vlink vlinks[MAX_VLINKS];
static int nvlinks, vfocus = -1;
static char varena[48000];
static usize vapos = 1;
static u8 *img;
static u32 img_w, img_h, img_y, page_h, img_sum;
static char vtitle[200], vurl[512];
static struct gfx gx;
static bool gx_open, view_mode, sum_ok;
static u32 vscroll;

static u32 num(const char **pp)
{
    const char *p = *pp;
    u32 v = 0;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') v = v * 10 + (u32)(*p++ - '0');
    *pp = p;
    return v;
}

static u32 vintern(const char *s, usize n)
{
    if (vapos + n + 1 > sizeof varena) return 0;
    memcpy(varena + vapos, s, n);
    varena[vapos + n] = 0;
    u32 o = (u32)vapos;
    vapos += n + 1;
    return o;
}

static bool parse_meta(const char *m, usize len, u32 *zlen)
{
    nvlinks = 0; vapos = 1; varena[0] = 0; *zlen = 0; img_sum = 0; vtitle[0] = 0; vurl[0] = 0;
    img_w = img_h = page_h = 0;
    int links = -1;
    usize i = 0;
    while (i < len) {
        usize s = i;
        while (i < len && m[i] != '\n') i++;
        usize l = i - s;
        const char *line = m + s;
        i++;
        if (links < 0) {
            const char *p;
            if (l > 2 && !memcmp(line, "w=", 2)) { p = line + 2; img_w = num(&p); }
            else if (l > 2 && !memcmp(line, "h=", 2)) { p = line + 2; img_h = num(&p); }
            else if (l > 2 && !memcmp(line, "y=", 2)) { p = line + 2; img_y = num(&p); }
            else if (l > 7 && !memcmp(line, "page_h=", 7)) { p = line + 7; page_h = num(&p); }
            else if (l > 5 && !memcmp(line, "zlen=", 5)) { p = line + 5; *zlen = num(&p); }
            else if (l > 4 && !memcmp(line, "sum=", 4)) { p = line + 4; img_sum = num(&p); }
            else if (l > 6 && !memcmp(line, "title=", 6)) { usize n = l - 6 < sizeof vtitle - 1 ? l - 6 : sizeof vtitle - 1; memcpy(vtitle, line + 6, n); vtitle[n] = 0; }
            else if (l > 4 && !memcmp(line, "url=", 4)) { usize n = l - 4 < sizeof vurl - 1 ? l - 4 : sizeof vurl - 1; memcpy(vurl, line + 4, n); vurl[n] = 0; }
            else if (l > 6 && !memcmp(line, "links=", 6)) { p = line + 6; links = (int)num(&p); }
        } else if (nvlinks < MAX_VLINKS) {
            const char *p = line;
            struct vlink v;
            v.x = num(&p); v.y = num(&p); v.w = num(&p); v.h = num(&p);
            while (*p == ' ') p++;
            usize ul = (usize)(line + l - p);
            v.url = vintern(p, ul);
            if (v.w && v.h && v.url) vlinks[nvlinks++] = v;
        }
    }
    return img_w > 0 && img_h > 0 && *zlen > 0 && (u64)img_w * img_h * 3 < 64ULL * 1024 * 1024;
}

static bool view_load(const char *url, u32 y)
{
    if (!gx_open) {
        int e = gfx_open(&gx);
        if (e) { snformat(message, sizeof message, "fb: %s", ao_errstr(e)); return false; }
        gx_open = true;
    }
    char req[640];
    usize n = (usize)snformat(req, sizeof req, "url=%s\nw=%u\nh=%u\ny=%u\n", url, gx.w, gx.h * 3, y);
    int e = aop_send(AOP_RENDER, req, n);
    if (e) { snformat(message, sizeof message, "kuldes: %s", ao_errstr(e)); return false; }
    u8 *zbuf = NULL;
    u32 zlen = 0, zgot = 0;
    bool meta_ok = false;
    for (;;) {
        u16 type;
        usize len;
        e = aop_recv(&type, &len);
        if (e) { snformat(message, sizeof message, "kapcsolat: %s", ao_errstr(e)); free(zbuf); return false; }
        if (type == AOP_RENDERED) {
            meta_ok = parse_meta((const char *)aop_payload, len, &zlen);
            if (meta_ok) zbuf = malloc(zlen);
            if (!zbuf) meta_ok = false;
        } else if (type == AOP_FILE) {
            usize p = 0, nls = 0;
            while (p < len && nls < 2) { if (aop_payload[p] == '\n') nls++; p++; }
            if (zbuf && nls == 2) {
                usize c = len - p;
                if (zgot + c > zlen) c = zlen - zgot;
                memcpy(zbuf + zgot, aop_payload + p, c);
                zgot += (u32)c;
            }
        } else if (type == AOP_END) {
            break;
        } else if (type == AOP_ERR) {
            snformat(message, sizeof message, "hid: %s", (char *)aop_payload);
            free(zbuf);
            return false;
        }
    }
    if (!meta_ok || zgot != zlen) { snformat(message, sizeof message, "hianyos kep (%u/%u bajt)", zgot, zlen); free(zbuf); return false; }
    usize need = (usize)img_w * img_h * 3;
    free(img);
    img = malloc(need);
    if (!img) { snformat(message, sizeof message, "nincs memoria a kephez (%lu KiB)", (u64)need / 1024); free(zbuf); return false; }
    isize got = inflate_zlib(zbuf, zlen, img, need);
    free(zbuf);
    if (got != (isize)need) { snformat(message, sizeof message, "kicsomagolas: %ld (vart %lu)", (i64)got, (u64)need); free(img); img = NULL; return false; }
    u32 s = 0;
    for (usize k = 0; k < need; k++) s += img[k];
    sum_ok = !img_sum || s == img_sum;
    if (!sum_ok) snformat(message, sizeof message, "ellenorzo osszeg hiba");
    vscroll = 0;
    vfocus = -1;
    view_valid = true;
    strlcpy(cur_url, vurl[0] ? vurl : url, sizeof cur_url);
    return true;
}

static void view_draw(void)
{
    u32 W = gx.w, H = gx.h, area = H - 2 * BAR;
    u32 bg = gfx_rgb(&gx, 20, 40, 90), fg = gfx_rgb(&gx, 255, 255, 255), dim = gfx_rgb(&gx, 170, 190, 220);
    u32 black = gfx_rgb(&gx, 0, 0, 0), amber = gfx_rgb(&gx, 255, 190, 40);
    gfx_fill(&gx, 0, 0, W, BAR, bg);
    gfx_text(&gx, 6, 2, 1, fg, bg, vtitle[0] ? vtitle : cur_url);
    char pos[64];
    snformat(pos, sizeof pos, "%u / %u", img_y + vscroll, page_h);
    u32 pw = gfx_text_width(pos, 1);
    gfx_text(&gx, W > pw + 8 ? W - pw - 8 : 0, 2, 1, dim, bg, pos);
    u32 rows = area;
    if (img && vscroll < img_h) {
        if (vscroll + rows > img_h) rows = img_h - vscroll;
        gfx_blit_rgb(&gx, 0, BAR, img, img_w * 3, 0, vscroll, img_w < W ? img_w : W, rows);
    } else {
        rows = 0;
    }
    if (rows < area) gfx_fill(&gx, 0, BAR + rows, W, area - rows, black);
    const char *lurl = "";
    if (vfocus >= 0 && vfocus < nvlinks) {
        struct vlink *l = &vlinks[vfocus];
        i64 sy = (i64)l->y - (i64)img_y - (i64)vscroll + BAR;
        i64 ey = sy + l->h;
        if (ey > BAR && sy < (i64)(BAR + area)) {
            if (sy < BAR) sy = BAR;
            if (ey > (i64)(BAR + area)) ey = BAR + area;
            gfx_frame(&gx, l->x, (u32)sy, l->w, (u32)(ey - sy), 2, amber);
        }
        lurl = varena + l->url;
    }
    gfx_fill(&gx, 0, H - BAR, W, BAR, bg);
    char bar[600];
    snformat(bar, sizeof bar, " v: lenyomat  nyilak/PgUp/PgDn  Tab: link  Enter: megnyit  Bksp: vissza  q: kilep   %s", lurl);
    gfx_text(&gx, 0, H - BAR + 2, 1, dim, bg, bar);
}

/* gorgetes a betoltott kepen belul; ha kilog, uj darab a hidrol */
static void view_scroll_to(i64 target)
{
    u32 area = gx.h - 2 * BAR;
    if (target < 0) target = 0;
    if (target >= (i64)img_y && target + area <= (i64)(img_y + img_h)) { vscroll = (u32)(target - img_y); return; }
    if (target + area > (i64)page_h) target = (i64)page_h - area;
    if (target < 0) target = 0;
    if (target >= (i64)img_y && target + area <= (i64)(img_y + img_h)) { vscroll = (u32)(target - img_y); return; }
    u32 ny = target > (i64)area ? (u32)(target - area) : 0;    /* egy kepernyonyi elozmeny is jojjon */
    if (view_load(cur_url, ny)) vscroll = (u32)(target - (i64)img_y);
    if (vscroll + area > img_h) vscroll = img_h > area ? img_h - area : 0;
}

static void view_enter(void)
{
    view_mode = true;
    if (!view_valid || strcmp(vurl, cur_url) != 0) view_load(cur_url, 0);
    if (gx_open) view_draw();
}

static void view_leave(void)
{
    view_mode = false;
    if (gx_open) { gfx_close(&gx); gx_open = false; }
    full_redraw = true;
    if (text_stale) {
        if (load(cur_url)) { cfocus = lfocus = ofocus = scroll_line = oscroll = lscroll = 0; col_focus = 1; layout(); }
        text_stale = false;
    }
}

static void view_open_url(const char *next)
{
    if (nhist == HIST_MAX) { memmove(hist[0], hist[1], sizeof hist - sizeof hist[0]); nhist--; }
    strlcpy(hist[nhist++], next, sizeof hist[0]);
    strlcpy(cur_url, next, sizeof cur_url);
    text_stale = true;
    view_load(cur_url, 0);
}

static void dump(void)
{
    ao_printf("# %s\n", title);
    for (int i = 0; i < nnodes; i++) {
        if (nodes[i].t == N_LINK) ao_printf("-> %s  (%s)\n", ntext(i), nlink(i));
        else ao_printf("[%s %u] %s\n", type_names[nodes[i].t], nodes[i].w, ntext(i));
    }
    ao_printf("(%d csomopont, %d forras)\n", nnodes, nsrcs);
}

int main(int argc, char **argv)
{
    bool do_dump = false, start_view = false;
    int first = 1;
    while (first < argc && argv[first][0] == '-') {
        if (strcmp(argv[first], "--dump") == 0) do_dump = true;
        else if (strcmp(argv[first], "--view") == 0) start_view = true;
        else break;
        first++;
    }
    if (argc <= first) { ao_puts("projector [--dump] [--view] CIM | KERDES\n"); return 1; }
    char q[512];
    usize ql = 0;
    for (int i = first; i < argc && ql + strlen(argv[i]) + 2 < sizeof q; i++) {
        usize l = strlen(argv[i]);
        if (ql) q[ql++] = ' ';
        memcpy(q + ql, argv[i], l); ql += l;
    }
    q[ql] = 0;

    struct sysinfo si;
    if (ao_sysinfo(&si) == 0 && si.con_cols >= 40 && si.con_rows >= 10) { W = si.con_cols; H = si.con_rows; }
    else { W = 80; H = 25; }
    if (H > MAX_ROWS) H = MAX_ROWS;

    int e = aop_connect("projector", true);
    if (e) return e;
    if (!open_query(q)) { aop_close(); return 5; }
    if (do_dump) {
        dump();
        if (start_view) {
            bool ok = view_load(cur_url, 0);
            if (gx_open) { gfx_close(&gx); gx_open = false; }
            if (ok) ao_printf("kep: %ux%u (y=%u, oldal %u), %d link, ellenorzo osszeg %s\n", img_w, img_h, img_y, page_h, nvlinks, sum_ok ? "ok" : "HIBA");
            else ao_printf("kep: hiba: %s\n", message);
        }
        aop_close();
        return 0;
    }
    strlcpy(hist[nhist++], q, sizeof hist[0]);
    if (start_view) view_enter();

    for (;;) {
        if (view_mode) {
            if (!gx_open) { view_mode = false; full_redraw = true; continue; }
            u32 area = gx.h - 2 * BAR;
            int k = read_key();
            if (k < 0) break;
            if (k == 'q' || k == K_ESC) break;
            else if (k == 'v') { view_leave(); continue; }
            else if (k == K_DOWN) view_scroll_to((i64)img_y + vscroll + 60);
            else if (k == K_UP) view_scroll_to((i64)img_y + vscroll - 60);
            else if (k == K_PGDN) view_scroll_to((i64)img_y + vscroll + area);
            else if (k == K_PGUP) view_scroll_to((i64)img_y + vscroll - area);
            else if (k == K_HOME) view_scroll_to(0);
            else if (k == K_END) view_scroll_to((i64)page_h - area);
            else if (k == '\t' && nvlinks) {
                vfocus = (vfocus + 1) % nvlinks;
                struct vlink *l = &vlinks[vfocus];
                i64 top = (i64)img_y + vscroll;
                if ((i64)l->y < top || (i64)l->y + l->h > top + area) view_scroll_to((i64)l->y - (i64)area / 3);
            } else if (k == '\n' && vfocus >= 0 && vfocus < nvlinks) {
                char next[512];
                strlcpy(next, varena + vlinks[vfocus].url, sizeof next);
                view_open_url(next);
            } else if (k == '\b' || k == K_LEFT) {
                if (nhist > 1) { nhist--; strlcpy(cur_url, hist[nhist - 1], sizeof cur_url); text_stale = true; view_load(cur_url, 0); }
            } else if (k == 'r') {
                view_load(cur_url, img_y);
            }
            if (gx_open) view_draw();
            continue;
        }
        draw();
        message[0] = 0;
        int k = read_key();
        if (k < 0) break;
        if (k == 'v') { view_enter(); continue; }
        if (searching) {
            if (k == '\n') { searching = false; find_next(); }
            else if (k == K_ESC) { searching = false; }
            else if (k == '\b') { usize l = strlen(search); if (l) { l--; while (l && is_cont(search[l])) l--; search[l] = 0; } }
            else if (k >= 32 && k < 0xE000) { usize l = strlen(search); if (l + key_rawn < sizeof search) { memcpy(search + l, key_raw, key_rawn); search[l + key_rawn] = 0; } }
            continue;
        }
        if (k == 'q' || k == K_ESC) break;
        if (k == '\t') { for (int t = 0; t < 3; t++) { col_focus = (col_focus + 1) % 3; if ((col_focus == 0 && noutline) || col_focus == 1 || (col_focus == 2 && nlinks)) break; } }
        else if (k == K_UP) { if (col_focus == 1 && cfocus > 0) cfocus--; else if (col_focus == 0 && ofocus > 0) ofocus--; else if (col_focus == 2 && lfocus > 0) lfocus--; ensure_visible(); }
        else if (k == K_DOWN) { if (col_focus == 1 && cfocus + 1 < ncontent) cfocus++; else if (col_focus == 0 && ofocus + 1 < noutline) ofocus++; else if (col_focus == 2 && lfocus + 1 < nlinks) lfocus++; ensure_visible(); }
        else if (k == K_PGDN) { scroll_line += (int)CR; if (scroll_line > nlines - 1) scroll_line = nlines > 0 ? nlines - 1 : 0; cfocus = node_at_line(scroll_line); col_focus = 1; }
        else if (k == K_PGUP) { scroll_line -= (int)CR; if (scroll_line < 0) scroll_line = 0; cfocus = node_at_line(scroll_line); col_focus = 1; }
        else if (k == K_HOME) { cfocus = 0; scroll_line = 0; col_focus = 1; }
        else if (k == K_END) { cfocus = ncontent ? ncontent - 1 : 0; col_focus = 1; ensure_visible(); }
        else if (k == '\n') {
            if (col_focus == 0 && noutline) {
                int target = outline[ofocus];
                for (int c = 0; c < ncontent; c++) if (content[c] == target) { cfocus = c; break; }
                col_focus = 1;
                scroll_line = node_first_line[cfocus];
                ensure_visible();
            } else {
                int i = -1;
                if (col_focus == 2 && nlinks) i = links[lfocus];
                else if (col_focus == 1 && ncontent && nodes[content[cfocus]].t == N_LINK) i = content[cfocus];
                if (i >= 0 && arena[nodes[i].to]) {
                    char next[512];
                    strlcpy(next, nlink(i), sizeof next);
                    if (nhist == HIST_MAX) { memmove(hist[0], hist[1], sizeof hist - sizeof hist[0]); nhist--; }
                    if (open_query(next)) strlcpy(hist[nhist++], next, sizeof hist[0]);
                    else { full_redraw = true; snformat(message, sizeof message, "nem sikerult: %s", next); }
                }
            }
        } else if (k == '\b' || k == K_LEFT) {
            if (nhist > 1) {
                nhist--;
                if (!open_query(hist[nhist - 1])) { full_redraw = true; snformat(message, sizeof message, "nem sikerult: %s", hist[nhist - 1]); }
            } else {
                snformat(message, sizeof message, "nincs hova visszalepni");
            }
        } else if (k == '/') { searching = true; search[0] = 0; }
        else if (k == 'n') { find_next(); }
        else if (k == 's') { save(); }
    }
    if (gx_open) { gfx_close(&gx); gx_open = false; }
    ao_puts("\x1b[2J\x1b[H");
    ao_printf("projector: %s (%d csomopont, %d link, %d forras)\n", title, nnodes, nlinks, nsrcs);
    aop_close();
    return 0;
}
