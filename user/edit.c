/* edit: teljes kepernyos szovegszerkeszto (nano-szeru), a shell 'edit FAJL' parancsa.
 * Szoveges konzolmodban fut (a billentyuzet-kiosztast a kernel alkalmazza), a specialis
 * billentyuk ESC-szekvenciakent, a Ctrl-kombinaciok vezerlokodkent (1..26) erkeznek.
 *
 * Billentyuk: nyilak, Home/End, PgUp/PgDn, Enter (a behuzast orokli), Tab (4 szokoz), Backspace, Del
 *   ^S ment   ^Q kilep (nem mentett valtozasnal ketszer)   ^F keres   ^N kovetkezo talalat
 *   ^G sorra ugras   ^K sor kivagasa (a PC vagolapjara is)   ^U beillesztes   ^A / ^E sor eleje / vege
 *   ^L ujrarajzolas   ^V a PC vagolapja a kurzorhoz (a kernel hozza a hidon at, nyersen, behuzas nelkul)
 *   F5 .py fajlnal: mentes, futtatas Pythonnal, hibanal ugras a sorra (a /tmp/pyerr.txt tracebackjebol)
 *
 * A sorok kulon pufferek (UTF-8 bajtok), a kurzor bajtpozicio; a kepernyon kodpontonkent egy cella,
 * a tab a kovetkezo 4-es oszlopig. Csak a valtozott sor rajzolodik ujra, gorgetesnel az egesz. */
#include "aolib.h"
#include "malloc.h"
#include "../kernel/lib/string.h"
#include "../kernel/lib/fmt.h"

#define TABW 4
#define MAX_FILE (4u * 1024 * 1024)
#define PATH_MAX 256

struct line { char *s; u32 len, cap; };

static struct line *L;
static u32 nl, lcap;
static char path[PATH_MAX];
static bool modified, quit_armed, full;
static u32 cy, cx, want_col;        /* kurzor: sor, bajt a sorban; kivant cella-oszlop fuggoleges mozgasnal */
static u32 top, leftc;              /* elso lathato sor; vizszintes eltolas cellakban */
static u32 rows, cols, trows, gutter;
static char msg[200];
static char *clip;
static u32 cliplen;
static char findbuf[128];

/* ---------------------------------------------------------------- kimenet */
static char out[65536];
static usize outn;

static void flush(void) { if (outn) { ao_write(1, out, outn); outn = 0; } }
static void out_ch(char c, void *ctx) { (void)ctx; if (outn >= sizeof out) flush(); out[outn++] = c; }
static void emit(const char *s, usize n) { for (usize i = 0; i < n; i++) out_ch(s[i], NULL); }
static void emits(const char *s) { emit(s, strlen(s)); }
static void emitf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vformat(out_ch, NULL, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- sorok */
static bool is_cont(char c) { return ((u8)c & 0xC0) == 0x80; }

static void line_ensure(struct line *l, u32 need)
{
    if (need <= l->cap) return;
    u32 cap = l->cap ? l->cap : 32;
    while (cap < need) cap *= 2;
    l->s = realloc(l->s, cap);
    l->cap = cap;
}

static void lines_ensure(u32 need)
{
    if (need <= lcap) return;
    u32 cap = lcap ? lcap : 64;
    while (cap < need) cap *= 2;
    L = realloc(L, cap * sizeof *L);
    lcap = cap;
}

static void line_insert(u32 at, const char *s, u32 n)
{
    lines_ensure(nl + 1);
    memmove(L + at + 1, L + at, (nl - at) * sizeof *L);
    L[at].s = NULL; L[at].len = 0; L[at].cap = 0;
    if (n) { line_ensure(&L[at], n); memcpy(L[at].s, s, n); L[at].len = n; }
    nl++;
}

static void line_delete(u32 at)
{
    free(L[at].s);
    memmove(L + at, L + at + 1, (nl - at - 1) * sizeof *L);
    nl--;
}

static u32 cp_next(const struct line *l, u32 i)
{
    if (i >= l->len) return l->len;
    i++;
    while (i < l->len && is_cont(l->s[i])) i++;
    return i;
}

static u32 cp_prev(const struct line *l, u32 i)
{
    if (i == 0) return 0;
    i--;
    while (i > 0 && is_cont(l->s[i])) i--;
    return i;
}

/* a [0, upto) bajtok szelessege cellakban */
static u32 cells_upto(const struct line *l, u32 upto)
{
    u32 c = 0;
    for (u32 i = 0; i < upto && i < l->len; i++) {
        if (l->s[i] == '\t') c += TABW - c % TABW;
        else if (!is_cont(l->s[i])) c++;
    }
    return c;
}

/* az elso bajt, ahol a cella-oszlop eleri a kertet (kodpont-hataron) */
static u32 byte_at_cells(const struct line *l, u32 cells)
{
    u32 c = 0, i = 0;
    while (i < l->len) {
        u32 w = l->s[i] == '\t' ? TABW - c % TABW : 1;
        if (c + w > cells) break;
        c += w;
        i = cp_next(l, i);
    }
    return i;
}

/* ---------------------------------------------------------------- szintaxis (Python) */
/* Bajtonkenti osztalyozas soronkent; a tripla idezojeles karakterlanc allapota a sor elejen az elozo
 * sorokbol adodik (vegigolvassuk oket, olcso). Csak .py fajlnal fut. */
enum { C_NORM = 0, C_KW, C_STR, C_COM, C_NUM, C_BUILTIN, C_ERR, C_MATCH };
static bool is_py;
static const char *const py_kw[] = {
    "def", "class", "if", "elif", "else", "for", "while", "return", "import", "from", "as", "in", "not", "and",
    "or", "try", "except", "finally", "with", "pass", "break", "continue", "lambda", "yield", "None", "True",
    "False", "global", "nonlocal", "del", "raise", "assert", "is", "async", "await", NULL };
static const char *const py_builtin[] = {
    "print", "input", "len", "range", "int", "str", "float", "list", "dict", "set", "tuple", "open", "abs", "min",
    "max", "sum", "sorted", "enumerate", "zip", "map", "filter", "type", "isinstance", "bool", "bytes", "round",
    "chr", "ord", "any", "all", "repr", "super", "object", "Exception", "self", NULL };

static bool ident_ch(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || (u8)c >= 0x80; }

static bool word_in(const char *s, u32 n, const char *const *list)
{
    for (; *list; list++)
        if (strlen(*list) == n && memcmp(s, *list, n) == 0) return true;
    return false;
}

/* cls[i] a sor i. bajtjanak osztalya (cls lehet NULL: csak az allapot kell); visszaadja a sor vegi
 * tripla-allapotot (0, '"' vagy '\'') */
static char classify(const struct line *l, char st, u8 *cls)
{
    const char *s = l->s;
    u32 n = l->len, i = 0;
    while (i < n) {
        if (st) {                                       /* tripla idezojeles karakterlancban vagyunk */
            u32 j = i;
            while (j + 2 < n && !(s[j] == st && s[j + 1] == st && s[j + 2] == st)) j++;
            if (j + 2 < n) { if (cls) memset(cls + i, C_STR, j + 3 - i); i = j + 3; st = 0; }
            else { if (cls) memset(cls + i, C_STR, n - i); i = n; }
            continue;
        }
        char c = s[i];
        if (c == '#') { if (cls) memset(cls + i, C_COM, n - i); break; }
        if (c == '"' || c == '\'') {
            if (i + 2 < n && s[i + 1] == c && s[i + 2] == c) { if (cls) memset(cls + i, C_STR, 3); i += 3; st = c; continue; }
            u32 j = i + 1;
            while (j < n && s[j] != c) { if (s[j] == '\\' && j + 1 < n) j++; j++; }
            if (j < n) j++;
            if (cls) memset(cls + i, C_STR, j - i);
            i = j;
            continue;
        }
        if (ident_ch(c) && !(c >= '0' && c <= '9')) {
            u32 j = i;
            while (j < n && ident_ch(s[j])) j++;
            u8 k = word_in(s + i, j - i, py_kw) ? C_KW : word_in(s + i, j - i, py_builtin) ? C_BUILTIN : C_NORM;
            if (cls) memset(cls + i, k, j - i);
            i = j;
            continue;
        }
        if (c >= '0' && c <= '9') {
            u32 j = i;
            while (j < n && (ident_ch(s[j]) || s[j] == '.')) j++;
            if (cls) memset(cls + i, C_NUM, j - i);
            i = j;
            continue;
        }
        if (cls) cls[i] = C_NORM;
        i++;
    }
    return st;
}

static char state_before(u32 idx)
{
    char st = 0;
    for (u32 i = 0; i < idx; i++) st = classify(&L[i], st, NULL);
    return st;
}

/* hibajelzes (F5 utan): sor, es szintaxishibanal a par nelkuli zarojel helye */
static u32 err_line;                /* 1-alapu, 0 = nincs */
static i64 err_bl = -1, err_bx;     /* a piros zarojel sora es bajtja */
/* a kurzor alatti zarojel parja */
static i64 m1_l = -1, m1_x, m2_l = -1, m2_x;

static bool is_open(char c) { return c == '(' || c == '[' || c == '{'; }
static bool is_close(char c) { return c == ')' || c == ']' || c == '}'; }
static char pair_of(char c) { return c == '(' ? ')' : c == '[' ? ']' : c == '{' ? '}' : c == ')' ? '(' : c == ']' ? '[' : '{'; }

/* a teljes fajl zarojelei: az elso hibas zaro, vagy az utolso nyitva maradt nyito -> err_bl/err_bx */
static void find_unbalanced(void)
{
    err_bl = -1;
    if (!is_py) return;
    struct { u32 l, x; char c; } stack[256];
    u32 sp = 0;
    char st = 0;
    static u8 cls[4096];
    for (u32 li = 0; li < nl; li++) {
        const struct line *l = &L[li];
        u32 n = l->len < sizeof cls ? l->len : (u32)sizeof cls;
        st = classify(l, st, cls);
        for (u32 i = 0; i < n; i++) {
            if (cls[i] != C_NORM) continue;
            char c = l->s[i];
            if (is_open(c)) { if (sp < 256) { stack[sp].l = li; stack[sp].x = i; stack[sp].c = c; sp++; } }
            else if (is_close(c)) {
                if (!sp || pair_of(stack[sp - 1].c) != c) { err_bl = li; err_bx = i; return; }
                sp--;
            }
        }
    }
    if (sp) { err_bl = stack[sp - 1].l; err_bx = stack[sp - 1].x; }
}

/* a kurzor alatti (vagy elotti) zarojel parja; a keresés karakterlancot/megjegyzest kihagy */
static void find_match(void)
{
    m1_l = m2_l = -1;
    if (!is_py || !nl) return;
    static u8 cls[4096];
    const struct line *l = &L[cy];
    u32 x = cx;
    if (!(x < l->len && (is_open(l->s[x]) || is_close(l->s[x])))) {
        if (x > 0 && (is_open(l->s[x - 1]) || is_close(l->s[x - 1]))) x--;
        else return;
    }
    char st0 = state_before(cy);
    u32 n = l->len < sizeof cls ? l->len : (u32)sizeof cls;
    classify(l, st0, cls);
    if (x >= n || cls[x] != C_NORM) return;
    char c = l->s[x], want = pair_of(c);
    int dir = is_open(c) ? 1 : -1;
    int depth = 0;
    i64 li = cy;
    i64 i = x;
    char st = st0;
    for (u32 steps = 0; steps < 2000; steps++) {
        const struct line *cl = &L[li];
        n = cl->len < sizeof cls ? cl->len : (u32)sizeof cls;
        if (li != (i64)cy) { st = dir > 0 ? st : state_before((u32)li); classify(cl, st, cls); }
        for (; i >= 0 && i < (i64)n; i += dir) {
            if (cls[i] != C_NORM) continue;
            char d = cl->s[i];
            if (d == c) depth++;
            else if (d == want && --depth == 0) { m1_l = cy; m1_x = x; m2_l = li; m2_x = i; return; }
        }
        if (dir > 0) { st = classify(cl, li == (i64)cy ? st0 : st, NULL); li++; if (li >= (i64)nl) return; i = 0; }
        else { li--; if (li < 0) return; i = (i64)L[li].len - 1; }
    }
}

/* ---------------------------------------------------------------- rajzolas */
/* minden sorozat 0-val kezd: a hatteres (hiba, par) osztalybol kilepve is tiszta lap */
static const char *const cls_seq[] = {
    [C_NORM] = "\x1b[0m", [C_KW] = "\x1b[0;94m", [C_STR] = "\x1b[0;33m", [C_COM] = "\x1b[0;90m", [C_NUM] = "\x1b[0;96m",
    [C_BUILTIN] = "\x1b[0;93m", [C_ERR] = "\x1b[0;41;97m", [C_MATCH] = "\x1b[0;100;97m",
};

static void draw_row(u32 r)
{
    u32 idx = top + r;
    emitf("\x1b[%u;1H", r + 2);
    if (idx >= nl) { emits("\x1b[90m~\x1b[0m\x1b[K"); return; }
    const struct line *l = &L[idx];
    char num[16];
    usize nn = snformat(num, sizeof num, "%u", idx + 1);
    emits(err_line == idx + 1 ? "\x1b[91m" : "\x1b[90m");
    for (u32 k = (u32)nn; k + 1 < gutter; k++) out_ch(' ', NULL);
    emits(num);
    emits(" \x1b[0m");
    static u8 cls[4096];
    u32 cn = 0;
    if (is_py) {
        cn = l->len < sizeof cls ? l->len : (u32)sizeof cls;
        classify(l, state_before(idx), cls);
    }
    u32 width = cols - gutter, c = 0;
    u8 cur = C_NORM;
    for (u32 i = 0; i < l->len; ) {
        u32 w = l->s[i] == '\t' ? TABW - c % TABW : 1;
        u32 n = cp_next(l, i) - i;
        if (c >= leftc + width) break;
        if (c + w > leftc) {
            u8 k = i < cn ? cls[i] : C_NORM;
            if ((i64)idx == err_bl && (i64)i == err_bx) k = C_ERR;
            else if (((i64)idx == m1_l && (i64)i == m1_x) || ((i64)idx == m2_l && (i64)i == m2_x)) k = C_MATCH;
            if (k != cur) { emits(cls_seq[k]); cur = k; }
            if (l->s[i] == '\t') { for (u32 k2 = c < leftc ? leftc : c; k2 < c + w && k2 < leftc + width; k2++) out_ch(' ', NULL); }
            else emit(l->s + i, n);
        }
        c += w;
        i += n;
    }
    if (cur != C_NORM) emits("\x1b[0m");
    emits("\x1b[K");
}

static void draw_all(void)
{
    for (u32 r = 0; r < trows; r++) draw_row(r);
}

static void pad_to(u32 used)
{
    for (u32 k = used; k < cols; k++) out_ch(' ', NULL);
}

static void draw_bars(void)
{
    char b[512];
    usize n = snformat(b, sizeof b, " edit  %s%s", path, modified ? "  [modositva]" : "");
    emits("\x1b[1;1H\x1b[44;97m");
    emit(b, n < cols ? n : cols);
    pad_to((u32)n);
    emits("\x1b[0m");
    emitf("\x1b[%u;1H\x1b[47;30m", rows);
    char right[64];
    usize rn = snformat(right, sizeof right, " sor %u/%u  oszlop %u ", cy + 1, nl, cells_upto(&L[cy], cx) + 1);
    if (msg[0]) n = snformat(b, sizeof b, " %s", msg);
    else n = snformat(b, sizeof b, " ^S ment  ^Q kilep  ^F keres  ^N kovetkezo  ^G sor  ^K kivag  ^U beilleszt  ^V PC vagolap  F5 futtat");
    if (n + rn > cols) n = cols > rn ? cols - rn : 0;
    emit(b, n);
    for (u32 k = (u32)n; k + rn < cols; k++) out_ch(' ', NULL);
    emit(right, rn < cols ? rn : cols);
    emits("\x1b[0m");
}

static void place_cursor(void)
{
    u32 col = gutter + cells_upto(&L[cy], cx) - leftc;
    emitf("\x1b[%u;%uH", cy - top + 2, col + 1);
}

static void scroll_into_view(void)
{
    if (cy < top) { top = cy; full = true; }
    if (cy >= top + trows) { top = cy - trows + 1; full = true; }
    u32 cc = cells_upto(&L[cy], cx), width = cols - gutter;
    if (cc < leftc) { leftc = cc; full = true; }
    if (cc >= leftc + width) { leftc = cc - width + 1; full = true; }
}

/* ---------------------------------------------------------------- billentyuk */
enum { K_UP = 0xE000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_ESC, K_PASTE_ON, K_PASTE_OFF, K_F5 };

static int read_key(void)
{
    u8 b[8];
    isize n = ao_read(0, b, sizeof b);
    if (n <= 0) return -1;
    if (b[0] == 0x1b) {
        if (n >= 6 && b[1] == '[' && b[2] == '2' && b[3] == '0' && b[5] == '~')   /* ESC[200~ / ESC[201~ */
            return b[4] == '0' ? K_PASTE_ON : K_PASTE_OFF;
        if (n >= 5 && b[1] == '[' && b[2] == '1' && b[3] == '5' && b[4] == '~') return K_F5;
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
    int need = (b[0] & 0xE0) == 0xC0 ? 1 : (b[0] & 0xF0) == 0xE0 ? 2 : 3;
    u32 cp = b[0] & (0x3F >> need);
    for (int i = 1; i <= need && i < n; i++) cp = (cp << 6) | (b[i] & 0x3F);
    return (int)cp;
}

static u32 utf8_put(u32 cp, char *o)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

/* egy sor bekerese az also sorban; Enter = igaz, Esc = hamis */
static bool prompt(const char *label, char *buf, usize cap)
{
    usize len = strlen(buf);
    for (;;) {
        emitf("\x1b[%u;1H\x1b[47;30m %s%s", rows, label, buf);
        usize used = 1 + strlen(label) + len;
        pad_to((u32)used);
        emitf("\x1b[0m\x1b[%u;%uH", rows, (u32)used + 1);
        flush();
        int k = read_key();
        if (k < 0 || k == K_ESC) return false;
        if (k == '\n') return true;
        if (k == '\b' || k == 0x7F) { while (len > 0) { len--; if (!is_cont(buf[len])) break; } buf[len] = 0; }
        else if (k >= 32 && k < 0xE000 && len + 4 < cap) { len += utf8_put((u32)k, buf + len); buf[len] = 0; }
    }
}

/* ---------------------------------------------------------------- szerkesztes */
static void insert_bytes(const char *s, u32 n)
{
    struct line *l = &L[cy];
    line_ensure(l, l->len + n);
    memmove(l->s + cx + n, l->s + cx, l->len - cx);
    memcpy(l->s + cx, s, n);
    l->len += n;
    cx += n;
    modified = true;
}

static void split_line(bool inherit_indent)
{
    struct line *l = &L[cy];
    u32 indent = 0;
    if (inherit_indent) while (indent < l->len && indent < cx && l->s[indent] == ' ') indent++;
    u32 tail = l->len - cx;
    char *tmp = malloc(indent + tail + 1);
    memset(tmp, ' ', indent);
    memcpy(tmp + indent, l->s + cx, tail);
    l->len = cx;
    line_insert(cy + 1, tmp, indent + tail);
    free(tmp);
    cy++;
    cx = indent;
    modified = true;
    full = true;
}

static void join_with_next(u32 at)
{
    if (at + 1 >= nl) return;
    struct line *a = &L[at], *b = &L[at + 1];
    line_ensure(a, a->len + b->len);
    memcpy(a->s + a->len, b->s, b->len);
    a->len += b->len;
    line_delete(at + 1);
    modified = true;
    full = true;
}

static void backspace(void)
{
    if (cx > 0) {
        struct line *l = &L[cy];
        u32 p = cp_prev(l, cx);
        memmove(l->s + p, l->s + cx, l->len - cx);
        l->len -= cx - p;
        cx = p;
        modified = true;
    } else if (cy > 0) {
        cx = L[cy - 1].len;
        join_with_next(cy - 1);
        cy--;
    }
}

static void del_forward(void)
{
    struct line *l = &L[cy];
    if (cx < l->len) {
        u32 e = cp_next(l, cx);
        memmove(l->s + cx, l->s + e, l->len - e);
        l->len -= e - cx;
        modified = true;
    } else {
        join_with_next(cy);
    }
}

/* szoveg a PC vagolapjara: /tmp/clip.txt + a clip-agent (agentd --file clip) a clip.cap jogaival */
static bool copy_to_pc(const char *s, u32 n)
{
    int fd = ao_open("/tmp/clip.txt", O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) return false;
    ao_write(fd, s, n);
    ao_write(fd, "\n", 1);
    ao_close(fd);
    static char manifest[1024];
    fd = ao_open("/etc/agents/clip.cap", O_READ);
    if (fd < 0) return false;
    isize r = ao_read(fd, manifest, sizeof manifest - 1);
    ao_close(fd);
    if (r <= 0) return false;
    manifest[r] = 0;
    char *argv[] = { "agentd", "--file", "clip", "clip", "/tmp/clip.txt", NULL };
    int pid = ao_spawn("/bin/agentd.aox", argv, manifest);
    if (pid < 0) pid = ao_spawn("/rd/bin/agentd.aox", argv, manifest);
    if (pid < 0) return false;
    int status = 1;
    ao_wait(pid, &status);
    return status == 0;
}

static void cut_line(void)
{
    struct line *l = &L[cy];
    free(clip);
    clip = malloc(l->len + 1);
    memcpy(clip, l->s, l->len);
    cliplen = l->len;
    if (nl == 1) l->len = 0;
    else { line_delete(cy); if (cy >= nl) cy = nl - 1; }
    cx = 0;
    modified = true;
    full = true;
    bool pc = copy_to_pc(clip, cliplen);
    snformat(msg, sizeof msg, "sor kivagva%s (^U beilleszti)", pc ? ", a PC vagolapjan is" : "");
}

static void paste_line(void)
{
    if (!clip) { snformat(msg, sizeof msg, "nincs kivagott sor"); return; }
    line_insert(cy, clip, cliplen);
    cy++;
    modified = true;
    full = true;
}

static bool find_from(u32 sy, u32 sx)
{
    usize n = strlen(findbuf);
    if (!n) return false;
    for (u32 pass = 0; pass <= nl; pass++) {
        u32 y = (sy + pass) % nl;
        u32 start = pass == 0 ? sx : 0;
        const struct line *l = &L[y];
        for (u32 i = start; i + n <= l->len; i++) {
            if (memcmp(l->s + i, findbuf, n) == 0) {
                cy = y; cx = i; want_col = cells_upto(l, cx);
                snformat(msg, sizeof msg, "talalat: %u. sor", y + 1);
                return true;
            }
        }
    }
    snformat(msg, sizeof msg, "nincs talalat: %s", findbuf);
    return false;
}

/* ---------------------------------------------------------------- fajl */
static bool load(void)
{
    struct stat st;
    if (ao_stat(path, &st) != 0) { line_insert(0, NULL, 0); snformat(msg, sizeof msg, "uj fajl"); return true; }
    if (st.type == 2) { ao_printf("edit: %s: konyvtar\n", path); return false; }
    if (st.size > MAX_FILE) { ao_printf("edit: %s: tul nagy (%u bajt)\n", path, st.size); return false; }
    int fd = ao_open(path, O_READ);
    if (fd < 0) { ao_printf("edit: %s: %s\n", path, ao_errstr(fd)); return false; }
    char *buf = malloc(st.size + 1);
    usize got = 0;
    while (got < st.size) {
        isize r = ao_read(fd, buf + got, st.size - got);
        if (r <= 0) break;
        got += (usize)r;
    }
    ao_close(fd);
    usize i = 0;
    while (i < got) {
        usize e = i;
        while (e < got && buf[e] != '\n') e++;
        usize l = e - i;
        if (l && buf[i + l - 1] == '\r') l--;
        line_insert(nl, buf + i, (u32)l);
        i = e + 1;
    }
    if (nl == 0) line_insert(0, NULL, 0);
    free(buf);
    snformat(msg, sizeof msg, "%u sor, %lu bajt", nl, (u64)got);
    return true;
}

static bool save(void)
{
    usize total = 0;
    for (u32 i = 0; i < nl; i++) total += L[i].len + 1;
    char *buf = malloc(total);
    usize p = 0;
    for (u32 i = 0; i < nl; i++) { memcpy(buf + p, L[i].s, L[i].len); p += L[i].len; buf[p++] = '\n'; }
    int fd = ao_open(path, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) { free(buf); snformat(msg, sizeof msg, "mentes: %s: %s", path, ao_errstr(fd)); return false; }
    usize done = 0;
    while (done < total) {
        usize chunk = total - done > 65536 ? 65536 : total - done;
        isize w = ao_write(fd, buf + done, chunk);
        if (w <= 0) { ao_close(fd); free(buf); snformat(msg, sizeof msg, "mentes: %s: %s", path, ao_errstr((int)w)); return false; }
        done += (usize)w;
    }
    ao_close(fd);
    free(buf);
    modified = false;
    snformat(msg, sizeof msg, "mentve: %s (%lu bajt, %u sor)", path, (u64)total, nl);
    return true;
}

/* ---------------------------------------------------------------- futtatas (F5) */
/* a fajl kiterjesztese .py: mentes, a python.aox futtatasa a python.cap jogaival a konzolon, majd
 * a /tmp/pyerr.txt tracebackjebol a hibas sorra ugras es az uzenet az allapotsorban */
static bool ends_with(const char *s, const char *suf)
{
    usize l = strlen(s), m = strlen(suf);
    return l >= m && strcmp(s + l - m, suf) == 0;
}

static void jump_to_error(void)
{
    int fd = ao_open("/tmp/pyerr.txt", O_READ);
    if (fd < 0) return;
    static char buf[4096];
    isize n = ao_read(fd, buf, sizeof buf - 1);
    ao_close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    /* az utolso 'File "<nev>", line N', ahol a nev a mi fajlunk (teljes ut vagy a fajlnev egyezik;
     * a python a kapott nevet irja, ami lehet relativ); az utolso nem ures sor a kivetel szovege */
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/' && p[1]) base = p + 1;
    usize bl = strlen(base);
    u32 line = 0;
    for (usize i = 0; i + 8 < (usize)n; i++) {
        if (memcmp(buf + i, "File \"", 6) != 0) continue;
        usize q = i + 6, qe = q;
        while (qe < (usize)n && buf[qe] != '"' && buf[qe] != '\n') qe++;
        if (qe >= (usize)n || buf[qe] != '"' || memcmp(buf + qe, "\", line ", 8) != 0) continue;
        usize fl = qe - q;
        bool ours = (fl == strlen(path) && memcmp(buf + q, path, fl) == 0) ||
                    (fl >= bl && memcmp(buf + qe - bl, base, bl) == 0 && (fl == bl || buf[qe - bl - 1] == '/'));
        if (!ours) continue;
        u32 v = 0;
        for (usize j = qe + 8; j < (usize)n && buf[j] >= '0' && buf[j] <= '9'; j++) v = v * 10 + (u32)(buf[j] - '0');
        if (v) line = v;
    }
    usize e = (usize)n;
    while (e > 0 && (buf[e - 1] == '\n' || buf[e - 1] == '\r')) e--;
    usize s = e;
    while (s > 0 && buf[s - 1] != '\n') s--;
    buf[e] = 0;
    if (line >= 1) { cy = line <= nl ? line - 1 : nl - 1; cx = 0; want_col = 0; }   /* a fajl vegen tuli sor: az utolso */
    err_line = line <= nl ? line : nl;
    if (memcmp(buf + s, "SyntaxError", 11) == 0) find_unbalanced();     /* a par nelkuli zarojel pirosan */
    snformat(msg, sizeof msg, "%u. sor: %s", line, buf + s);
}

static void run_file(void)
{
    if (!ends_with(path, ".py")) { snformat(msg, sizeof msg, "F5: csak .py fajlt futtat (python)"); return; }
    struct stat st;
    if (ao_stat("/state/bin/python.aox", &st) != 0) { snformat(msg, sizeof msg, "F5: nincs python (fetch python.aox /state/bin)"); return; }
    if (modified && !save()) return;
    static char manifest[1024];
    int fd = ao_open("/etc/agents/python.cap", O_READ);
    if (fd < 0) { snformat(msg, sizeof msg, "F5: nincs /etc/agents/python.cap"); return; }
    isize r = ao_read(fd, manifest, sizeof manifest - 1);
    ao_close(fd);
    if (r <= 0) return;
    manifest[r] = 0;
    emits("\x1b[2J\x1b[H");
    emitf("--- %s futtatasa (F5) ---\n", path);
    flush();
    ao_unlink("/tmp/pyerr.txt");
    err_line = 0;
    err_bl = -1;
    char *argv[] = { "python", path, NULL };
    int pid = ao_spawn("/state/bin/python.aox", argv, manifest);
    int status = -1;
    if (pid < 0) ao_printf("python: inditas: %s\n", ao_errstr(pid));
    else ao_wait(pid, &status);
    ao_printf("--- vege (rc=%d) --- barmely billentyu: vissza a szerkesztobe\n", status);
    read_key();
    msg[0] = 0;
    jump_to_error();
    if (!msg[0]) snformat(msg, sizeof msg, "lefutott, rc=%d", status);
    full = true;
}

/* ---------------------------------------------------------------- fociklus */
/* sorszam-oszlop: a legnagyobb sorszam szamjegyei + egy szokoz, legalabb 4 */
static u32 gutter_for(u32 n)
{
    u32 g = 2;
    for (; n >= 10; n /= 10) g++;
    return g < 4 ? 4 : g;
}

static void move_vert(int dy)
{
    if (dy < 0 && cy < (u32)-dy) cy = 0;
    else if (dy < 0) cy += (u32)dy;
    else cy = cy + (u32)dy >= nl ? nl - 1 : cy + (u32)dy;
    cx = byte_at_cells(&L[cy], want_col);
}

int main(int argc, char **argv)
{
    if (argc < 2) { ao_puts("edit FAJL\n"); return 1; }
    strlcpy(path, argv[1], sizeof path);
    is_py = ends_with(path, ".py");
    struct sysinfo si;
    rows = 25; cols = 80;
    if (ao_sysinfo(&si) == 0 && si.con_rows >= 5 && si.con_cols >= 20) { rows = si.con_rows; cols = si.con_cols; }
    trows = rows - 2;
    if (!load()) return 1;
    gutter = gutter_for(nl);
    emits("\x1b[2J");
    full = true;
    bool paste_mode = false, paste_last_nl = false;   /* Ctrl+V: a kernel a PC vagolapjat ESC[200~ ... ESC[201~ kozott adja */
    u32 paste_lines = 0;
    for (;;) {
        if (!paste_mode) {
            scroll_into_view();
            i64 om1 = m1_l, om2 = m2_l;
            find_match();
            if (full) { draw_all(); full = false; }
            else {
                draw_row(cy - top);
                /* a zarojel-par masik sora: a regi es az uj kiemeles frissitese */
                i64 rows_[4] = { om1, om2, m1_l, m2_l };
                for (int q = 0; q < 4; q++)
                    if (rows_[q] >= 0 && rows_[q] != (i64)cy && rows_[q] >= (i64)top && rows_[q] < (i64)(top + trows))
                        draw_row((u32)rows_[q] - top);
            }
            draw_bars();
            place_cursor();
            flush();
        }
        int k = read_key();
        if (k < 0) break;
        bool keep_msg = false;
        if (k != 17) quit_armed = false;
        if (paste_mode) {
            /* nyers beillesztes: nincs behuzas-orokles, a tab is marad */
            if (k == K_PASTE_OFF) {
                paste_mode = false;
                full = true;
                want_col = cells_upto(&L[cy], cx);
                snformat(msg, sizeof msg, "beillesztve: %u sor", paste_lines + (paste_last_nl ? 0 : 1));
            } else if (k == '\n') { split_line(false); paste_lines++; paste_last_nl = true; }
            else if (k == '\t') { insert_bytes("\t", 1); paste_last_nl = false; }
            else if (k >= 32 && k < 0xE000) { char enc[4]; u32 n = utf8_put((u32)k, enc); insert_bytes(enc, n); paste_last_nl = false; }
            continue;
        }
        switch (k) {
        case K_PASTE_ON: paste_mode = true; paste_lines = 0; paste_last_nl = false; break;
        case K_PASTE_OFF: break;
        case K_F5: run_file(); keep_msg = true; break;
        case K_UP: move_vert(-1); break;
        case K_DOWN: move_vert(1); break;
        case K_PGUP: move_vert(-(int)trows); if (top >= trows) top -= trows; else top = 0; full = true; break;
        case K_PGDN: move_vert((int)trows); if (top + trows < nl) top += trows; full = true; break;
        case K_LEFT:
            if (cx > 0) cx = cp_prev(&L[cy], cx);
            else if (cy > 0) { cy--; cx = L[cy].len; }
            want_col = cells_upto(&L[cy], cx);
            break;
        case K_RIGHT:
            if (cx < L[cy].len) cx = cp_next(&L[cy], cx);
            else if (cy + 1 < nl) { cy++; cx = 0; }
            want_col = cells_upto(&L[cy], cx);
            break;
        case K_HOME: case 1: cx = 0; want_col = 0; break;
        case K_END: case 5: cx = L[cy].len; want_col = cells_upto(&L[cy], cx); break;
        case '\n': split_line(true); want_col = cells_upto(&L[cy], cx); break;
        case '\b': case 0x7F: backspace(); want_col = cells_upto(&L[cy], cx); break;
        case K_DEL: del_forward(); break;
        case '\t': {
            u32 c = cells_upto(&L[cy], cx), n = TABW - c % TABW;
            insert_bytes("    ", n);
            want_col = cells_upto(&L[cy], cx);
            break;
        }
        case 19: save(); keep_msg = true; break;
        case 17:
            if (modified && !quit_armed) { quit_armed = true; snformat(msg, sizeof msg, "nem mentett valtozasok: ^S ment, ^Q ujra = kilepes mentes nelkul"); keep_msg = true; break; }
            goto done;
        case 6:
            if (prompt("Keres: ", findbuf, sizeof findbuf)) find_from(cy, cx + 1);
            keep_msg = true;
            full = true;
            break;
        case 14: find_from(cy, cx + 1); keep_msg = true; break;
        case 7: {
            char num[16] = "";
            if (prompt("Sor: ", num, sizeof num)) {
                u32 v = 0;
                for (const char *p = num; *p >= '0' && *p <= '9'; p++) v = v * 10 + (u32)(*p - '0');
                if (v >= 1 && v <= nl) { cy = v - 1; cx = 0; want_col = 0; }
                else { snformat(msg, sizeof msg, "nincs ilyen sor (1..%u)", nl); keep_msg = true; }
            }
            full = true;
            break;
        }
        case 11: cut_line(); keep_msg = true; want_col = 0; break;
        case 21: paste_line(); keep_msg = true; break;
        case 12: full = true; break;
        case K_ESC: err_line = 0; err_bl = -1; full = true; break;      /* a hibajeloles torlese */
        default:
            if (k >= 32 && k < 0xE000) {
                char enc[4];
                u32 n = utf8_put((u32)k, enc);
                insert_bytes(enc, n);
                want_col = cells_upto(&L[cy], cx);
            }
            break;
        }
        if (!keep_msg) msg[0] = 0;
        if (gutter_for(nl) != gutter) { gutter = gutter_for(nl); full = true; }
    }
done:
    emits("\x1b[2J\x1b[H");
    flush();
    ao_printf("edit: %s, %u sor%s\n", path, nl, modified ? ", nem mentett valtozasok eldobva" : "");
    return 0;
}
