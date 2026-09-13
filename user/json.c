#include "json.h"

void jp_init(struct jp *j, const char *text, usize n) { j->p = text; j->end = text + n; }

static void ws(struct jp *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) j->p++;
}

int jp_peek(struct jp *j)
{
    ws(j);
    return j->p < j->end ? (u8)*j->p : -1;
}

bool jp_expect(struct jp *j, char c)
{
    if (jp_peek(j) != (u8)c) return false;
    j->p++;
    return true;
}

static int hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void put(char *out, usize cap, usize *n, char c)
{
    if (*n + 1 < cap) out[*n] = c;
    (*n)++;
}

static void put_cp(char *out, usize cap, usize *n, u32 cp)
{
    if (cp < 0x80) { put(out, cap, n, (char)cp); return; }
    if (cp < 0x800) { put(out, cap, n, (char)(0xC0 | (cp >> 6))); put(out, cap, n, (char)(0x80 | (cp & 0x3F))); return; }
    if (cp < 0x10000) {
        put(out, cap, n, (char)(0xE0 | (cp >> 12))); put(out, cap, n, (char)(0x80 | ((cp >> 6) & 0x3F)));
        put(out, cap, n, (char)(0x80 | (cp & 0x3F)));
        return;
    }
    put(out, cap, n, (char)(0xF0 | (cp >> 18))); put(out, cap, n, (char)(0x80 | ((cp >> 12) & 0x3F)));
    put(out, cap, n, (char)(0x80 | ((cp >> 6) & 0x3F))); put(out, cap, n, (char)(0x80 | (cp & 0x3F)));
}

bool jp_string(struct jp *j, char *out, usize cap, usize *len)
{
    if (!jp_expect(j, '"')) return false;
    usize n = 0;
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c != '\\') { put(out, cap, &n, c); continue; }
        if (j->p >= j->end) return false;
        char e = *j->p++;
        switch (e) {
        case 'n': put(out, cap, &n, '\n'); break;
        case 't': put(out, cap, &n, '\t'); break;
        case 'r': put(out, cap, &n, '\r'); break;
        case 'b': case 'f': put(out, cap, &n, ' '); break;
        case 'u': {
            if (j->end - j->p < 4) return false;
            u32 cp = 0;
            for (int i = 0; i < 4; i++) { int h = hexv(j->p[i]); if (h < 0) return false; cp = cp * 16 + (u32)h; }
            j->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                u32 lo = 0;
                for (int i = 0; i < 4; i++) { int h = hexv(j->p[2 + i]); if (h < 0) return false; lo = lo * 16 + (u32)h; }
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); j->p += 6; }
            }
            put_cp(out, cap, &n, cp);
            break;
        }
        default: put(out, cap, &n, e); break;      /* \" \\ \/ */
        }
    }
    if (j->p >= j->end) return false;
    j->p++;                                         /* zaro idezojel */
    if (cap) out[n < cap ? n : cap - 1] = 0;
    if (len) *len = n < cap ? n : cap - 1;
    return true;
}

bool jp_number(struct jp *j, i64 *v)
{
    ws(j);
    bool neg = false;
    if (j->p < j->end && *j->p == '-') { neg = true; j->p++; }
    if (j->p >= j->end || *j->p < '0' || *j->p > '9') return false;
    i64 x = 0;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') x = x * 10 + (*j->p++ - '0');
    if (j->p < j->end && *j->p == '.') { j->p++; while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++; }
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
        j->p++;
        if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    }
    *v = neg ? -x : x;
    return true;
}

bool jp_skip(struct jp *j)
{
    int c = jp_peek(j);
    if (c == '"') { return jp_string(j, NULL, 0, NULL); }
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        j->p++;
        if (jp_expect(j, close)) return true;
        for (;;) {
            if (c == '{') { if (!jp_key(j, NULL, 0)) return false; }
            if (!jp_skip(j)) return false;
            if (jp_expect(j, ',')) continue;
            return jp_expect(j, close);
        }
    }
    if (c == '-' || (c >= '0' && c <= '9')) { i64 v; return jp_number(j, &v); }
    /* true / false / null */
    while (j->p < j->end && ((*j->p >= 'a' && *j->p <= 'z'))) j->p++;
    return true;
}

bool jp_key(struct jp *j, char *key, usize cap)
{
    if (!jp_string(j, key, cap, NULL)) return false;
    return jp_expect(j, ':');
}
