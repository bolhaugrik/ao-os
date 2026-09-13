#include "crypto.h"
#include "aolib.h"

static inline u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static inline void st32(u8 *p, u32 v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24; }
static inline void st64(u8 *p, u64 v) { st32(p, (u32)v); st32(p + 4, (u32)(v >> 32)); }
static inline u32 rotl(u32 v, int c) { return (v << c) | (v >> (32 - c)); }

#define QR(a, b, c, d) \
    a += b; d ^= a; d = rotl(d, 16); \
    c += d; b ^= c; b = rotl(b, 12); \
    a += b; d ^= a; d = rotl(d, 8);  \
    c += d; b ^= c; b = rotl(b, 7);

static void rounds(u32 s[16])
{
    for (int i = 0; i < 10; i++) {
        QR(s[0], s[4], s[8], s[12]) QR(s[1], s[5], s[9], s[13]) QR(s[2], s[6], s[10], s[14]) QR(s[3], s[7], s[11], s[15])
        QR(s[0], s[5], s[10], s[15]) QR(s[1], s[6], s[11], s[12]) QR(s[2], s[7], s[8], s[13]) QR(s[3], s[4], s[9], s[14])
    }
}

static void init_state(u32 s[16], const u8 key[32])
{
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = le32(key + i * 4);
}

void chacha20_block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64])
{
    u32 s[16], w[16];
    init_state(s, key);
    s[12] = counter;
    s[13] = le32(nonce); s[14] = le32(nonce + 4); s[15] = le32(nonce + 8);
    memcpy(w, s, sizeof w);
    rounds(w);
    for (int i = 0; i < 16; i++) st32(out + i * 4, w[i] + s[i]);
}

void chacha20_xor(const u8 key[32], u32 counter, const u8 nonce[12], const u8 *in, u8 *out, usize n)
{
    u8 ks[64];
    for (usize off = 0; off < n; off += 64) {
        chacha20_block(key, counter + (u32)(off / 64), nonce, ks);
        usize c = n - off < 64 ? n - off : 64;
        for (usize i = 0; i < c; i++) out[off + i] = in[off + i] ^ ks[i];
    }
}

void hchacha20(const u8 key[32], const u8 nonce16[16], u8 out[32])
{
    u32 s[16];
    init_state(s, key);
    for (int i = 0; i < 4; i++) s[12 + i] = le32(nonce16 + i * 4);
    rounds(s);
    for (int i = 0; i < 4; i++) st32(out + i * 4, s[i]);
    for (int i = 0; i < 4; i++) st32(out + 16 + i * 4, s[12 + i]);
}

/* poly1305-donna, 32 bites limbek */
void poly1305_mac(const u8 key[32], const u8 *m, usize n, u8 tag[16])
{
    u32 r0 = le32(key + 0) & 0x3ffffff;
    u32 r1 = (le32(key + 3) >> 2) & 0x3ffff03;
    u32 r2 = (le32(key + 6) >> 4) & 0x3ffc0ff;
    u32 r3 = (le32(key + 9) >> 6) & 0x3f03fff;
    u32 r4 = (le32(key + 12) >> 8) & 0x00fffff;
    u32 s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    u32 h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;

    while (n) {
        u8 blk[16] = { 0 };
        usize c = n < 16 ? n : 16;
        memcpy(blk, m, c);
        u32 hibit = 1u << 24;
        if (c < 16) { blk[c] = 1; hibit = 0; }
        h0 += le32(blk + 0) & 0x3ffffff;
        h1 += (le32(blk + 3) >> 2) & 0x3ffffff;
        h2 += (le32(blk + 6) >> 4) & 0x3ffffff;
        h3 += (le32(blk + 9) >> 6) & 0x3ffffff;
        h4 += (le32(blk + 12) >> 8) | hibit;

        u64 d0 = (u64)h0 * r0 + (u64)h1 * s4 + (u64)h2 * s3 + (u64)h3 * s2 + (u64)h4 * s1;
        u64 d1 = (u64)h0 * r1 + (u64)h1 * r0 + (u64)h2 * s4 + (u64)h3 * s3 + (u64)h4 * s2;
        u64 d2 = (u64)h0 * r2 + (u64)h1 * r1 + (u64)h2 * r0 + (u64)h3 * s4 + (u64)h4 * s3;
        u64 d3 = (u64)h0 * r3 + (u64)h1 * r2 + (u64)h2 * r1 + (u64)h3 * r0 + (u64)h4 * s4;
        u64 d4 = (u64)h0 * r4 + (u64)h1 * r3 + (u64)h2 * r2 + (u64)h3 * r1 + (u64)h4 * r0;
        u32 cc;
        cc = (u32)(d0 >> 26); h0 = (u32)d0 & 0x3ffffff; d1 += cc;
        cc = (u32)(d1 >> 26); h1 = (u32)d1 & 0x3ffffff; d2 += cc;
        cc = (u32)(d2 >> 26); h2 = (u32)d2 & 0x3ffffff; d3 += cc;
        cc = (u32)(d3 >> 26); h3 = (u32)d3 & 0x3ffffff; d4 += cc;
        cc = (u32)(d4 >> 26); h4 = (u32)d4 & 0x3ffffff; h0 += cc * 5;
        cc = h0 >> 26; h0 &= 0x3ffffff; h1 += cc;
        m += c;
        n -= c;
    }

    u32 c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

    u32 g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    u32 g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    u32 g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    u32 g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    u32 g4 = h4 + c - (1u << 26);
    u32 mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;

    u32 o0 = h0 | (h1 << 26);
    u32 o1 = (h1 >> 6) | (h2 << 20);
    u32 o2 = (h2 >> 12) | (h3 << 14);
    u32 o3 = (h3 >> 18) | (h4 << 8);
    u64 f;
    f = (u64)o0 + le32(key + 16); o0 = (u32)f;
    f = (u64)o1 + le32(key + 20) + (f >> 32); o1 = (u32)f;
    f = (u64)o2 + le32(key + 24) + (f >> 32); o2 = (u32)f;
    f = (u64)o3 + le32(key + 28) + (f >> 32); o3 = (u32)f;
    st32(tag, o0); st32(tag + 4, o1); st32(tag + 8, o2); st32(tag + 12, o3);
}

/* Poly1305 bemenet az AEAD-hez: aad || pad || ct || pad || len(aad) || len(ct) */
static void aead_tag(const u8 key[32], const u8 nonce[12], const u8 *aad, usize aadn,
                     const u8 *ct, usize n, u8 tag[16])
{
    u8 otk[64];
    chacha20_block(key, 0, nonce, otk);
    /* a poly1305-ot darabokban etetjuk: egyszeru megoldas: osszefuzott puffer a veremben (max 64 KiB) */
    static u8 buf[70000];
    usize p = 0;
    memcpy(buf + p, aad, aadn); p += aadn;
    while (p % 16) buf[p++] = 0;
    memcpy(buf + p, ct, n); p += n;
    while (p % 16) buf[p++] = 0;
    st64(buf + p, (u64)aadn); p += 8;
    st64(buf + p, (u64)n); p += 8;
    poly1305_mac(otk, buf, p, tag);
}

void aead_encrypt(const u8 key[32], const u8 nonce[12], const u8 *aad, usize aadn,
                  const u8 *pt, usize n, u8 *out)
{
    chacha20_xor(key, 1, nonce, pt, out, n);
    aead_tag(key, nonce, aad, aadn, out, n, out + n);
}

bool aead_decrypt(const u8 key[32], const u8 nonce[12], const u8 *aad, usize aadn,
                  const u8 *in, usize n, u8 *out)
{
    if (n < 16) return false;
    u8 tag[16];
    aead_tag(key, nonce, aad, aadn, in, n - 16, tag);
    u8 diff = 0;
    for (int i = 0; i < 16; i++) diff |= tag[i] ^ in[n - 16 + i];
    if (diff) return false;
    chacha20_xor(key, 1, nonce, in, out, n - 16);
    return true;
}

/* ---------------------------------------------------------------- csatorna */
void aochan_init(struct aochan *c, const u8 psk[32], const u8 cnonce[8], const u8 snonce[8], bool is_server)
{
    u8 n16[16];
    memcpy(n16, cnonce, 8);
    memcpy(n16 + 8, snonce, 8);
    hchacha20(psk, n16, c->key);
    memcpy(c->tx_dir, is_server ? "srv" : "cli", 4);
    memcpy(c->rx_dir, is_server ? "cli" : "srv", 4);
    c->tx_ctr = c->rx_ctr = 0;
}

void aochan_seal(struct aochan *c, const u8 hdr[12], const u8 *pt, usize n, u8 *out)
{
    u8 nonce[12];
    memcpy(nonce, c->tx_dir, 4);
    st64(nonce + 4, c->tx_ctr++);
    aead_encrypt(c->key, nonce, hdr, 12, pt, n, out);
}

bool aochan_open(struct aochan *c, const u8 hdr[12], const u8 *ct, usize n, u8 *out)
{
    u8 nonce[12];
    memcpy(nonce, c->rx_dir, 4);
    st64(nonce + 4, c->rx_ctr);
    if (!aead_decrypt(c->key, nonce, hdr, 12, ct, n, out)) return false;
    c->rx_ctr++;
    return true;
}
