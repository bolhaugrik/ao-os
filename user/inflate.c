/* Deflate-kicsomagolo a puff.c (Mark Adler) algoritmusa nyoman: kanonikus Huffman-kodok
 * hossz-szamlalokkal, bitenkenti dekodolassal. Lassabb a tablazatosnal, de kicsi es biztos. */
#include "inflate.h"
#include "aolib.h"      /* hibakodok */

struct st {
    const u8 *in; usize inlen, inpos;
    u8 *out; usize outcap, outpos;
    u32 bitbuf; int bitcnt;
    bool err;
};

struct huff { u16 count[16]; u16 symbol[320]; };

static u32 bits(struct st *s, int n)
{
    u32 v = s->bitbuf;
    while (s->bitcnt < n) {
        if (s->inpos >= s->inlen) { s->err = true; return 0; }
        v |= (u32)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> n;
    s->bitcnt -= n;
    return v & ((1u << n) - 1);
}

static int decode(struct st *s, const struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= (int)bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* kodhosszakbol kanonikus tabla; 0 = ok, <0 = tulterhelt, >0 = hianyos (megengedett) */
static int construct(struct huff *h, const u8 *length, int n)
{
    for (int i = 0; i <= 15; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    u16 offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (u16)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++) if (length[i]) h->symbol[offs[length[i]]++] = (u16)i;
    return left;
}

static const u16 lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u16 lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const u16 dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const u16 dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static bool put(struct st *s, u8 c)
{
    if (s->outpos >= s->outcap) { s->err = true; return false; }
    s->out[s->outpos++] = c;
    return true;
}

static bool codes(struct st *s, const struct huff *lencode, const struct huff *distcode)
{
    for (;;) {
        int sym = decode(s, lencode);
        if (sym < 0) return false;
        if (sym < 256) { if (!put(s, (u8)sym)) return false; continue; }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        u32 len = lbase[sym] + bits(s, lext[sym]);
        int ds = decode(s, distcode);
        if (ds < 0 || ds >= 30) return false;
        u32 dist = dbase[ds] + bits(s, dext[ds]);
        if (s->err || dist > s->outpos) return false;
        while (len--) { if (!put(s, s->out[s->outpos - dist])) return false; }
    }
}

static bool stored(struct st *s)
{
    s->bitbuf = 0; s->bitcnt = 0;
    if (s->inpos + 4 > s->inlen) return false;
    u32 len = s->in[s->inpos] | (u32)s->in[s->inpos + 1] << 8;
    u32 nlen = s->in[s->inpos + 2] | (u32)s->in[s->inpos + 3] << 8;
    s->inpos += 4;
    if (len != (~nlen & 0xFFFF)) return false;
    if (s->inpos + len > s->inlen || s->outpos + len > s->outcap) return false;
    for (u32 i = 0; i < len; i++) s->out[s->outpos++] = s->in[s->inpos++];
    return true;
}

static bool fixed(struct st *s)
{
    static struct huff lencode, distcode;
    static bool built;
    if (!built) {
        u8 lengths[288];
        int i = 0;
        for (; i < 144; i++) lengths[i] = 8;
        for (; i < 256; i++) lengths[i] = 9;
        for (; i < 280; i++) lengths[i] = 7;
        for (; i < 288; i++) lengths[i] = 8;
        construct(&lencode, lengths, 288);
        for (i = 0; i < 30; i++) lengths[i] = 5;
        construct(&distcode, lengths, 30);
        built = true;
    }
    return codes(s, &lencode, &distcode);
}

static bool dynamic(struct st *s)
{
    static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    u8 lengths[320];
    struct huff lencode, distcode;
    int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1, ncode = (int)bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) return false;
    int index = 0;
    for (; index < ncode; index++) lengths[order[index]] = (u8)bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (construct(&lencode, lengths, 19) != 0) return false;
    index = 0;
    while (index < nlen + ndist) {
        int sym = decode(s, &lencode);
        if (sym < 0) return false;
        if (sym < 16) { lengths[index++] = (u8)sym; continue; }
        u8 len = 0;
        int rep;
        if (sym == 16) { if (index == 0) return false; len = lengths[index - 1]; rep = 3 + (int)bits(s, 2); }
        else if (sym == 17) rep = 3 + (int)bits(s, 3);
        else rep = 11 + (int)bits(s, 7);
        if (s->err || index + rep > nlen + ndist) return false;
        while (rep--) lengths[index++] = len;
    }
    if (lengths[256] == 0) return false;
    int e = construct(&lencode, lengths, nlen);
    if (e < 0 || (e > 0 && nlen - lencode.count[0] != 1)) return false;
    e = construct(&distcode, lengths + nlen, ndist);
    if (e < 0 || (e > 0 && ndist - distcode.count[0] != 1)) return false;
    return codes(s, &lencode, &distcode);
}

isize inflate_raw(const u8 *in, usize inlen, u8 *out, usize outcap)
{
    struct st s = { in, inlen, 0, out, outcap, 0, 0, 0, false };
    int last;
    do {
        last = (int)bits(&s, 1);
        int type = (int)bits(&s, 2);
        if (s.err) return E_INVAL;
        bool ok = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : false;
        if (!ok || s.err) return s.outpos >= s.outcap ? E_LIMIT : E_INVAL;
    } while (!last);
    return (isize)s.outpos;
}

isize inflate_zlib(const u8 *in, usize inlen, u8 *out, usize outcap)
{
    if (inlen < 6 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0) return E_INVAL;
    return inflate_raw(in + 2, inlen - 2, out, outcap);
}
