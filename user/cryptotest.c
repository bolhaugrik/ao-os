/* RFC 8439 tesztvektorok az OS-en futtatva: a C-implementacio ellenorzese. */
#include "aolib.h"
#include "crypto.h"

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
static void unhex(const char *s, u8 *out, usize n) { for (usize i = 0; i < n; i++) out[i] = (u8)(hexval(s[2 * i]) * 16 + hexval(s[2 * i + 1])); }
static bool eq(const u8 *a, const u8 *b, usize n) { return memcmp(a, b, n) == 0; }

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int fail = 0;
    u8 key[32], nonce[12], exp[64], out[256], buf[256];

    /* chacha20 block, RFC 8439 2.4.2 (counter 1) */
    for (int i = 0; i < 32; i++) key[i] = (u8)i;
    unhex("000000000000004a00000000", nonce, 12);
    chacha20_block(key, 1, nonce, out);
    unhex("224f51f3401bd9e12fde276fb8631ded", exp, 16);
    ao_printf("chacha20 block:  %s\n", eq(out, exp, 16) ? "OK" : "HIBA"); fail += !eq(out, exp, 16);

    /* poly1305, RFC 8439 2.5.2 */
    unhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", key, 32);
    const char *msg = "Cryptographic Forum Research Group";
    poly1305_mac(key, (const u8 *)msg, strlen(msg), out);
    unhex("a8061dc1305136c6c22b8baf0c0127a9", exp, 16);
    ao_printf("poly1305:        %s\n", eq(out, exp, 16) ? "OK" : "HIBA"); fail += !eq(out, exp, 16);

    /* AEAD, RFC 8439 2.8.2 */
    unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key, 32);
    unhex("070000004041424344454647", nonce, 12);
    u8 aad[12];
    unhex("50515253c0c1c2c3c4c5c6c7", aad, 12);
    const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    usize n = strlen(pt);
    aead_encrypt(key, nonce, aad, 12, (const u8 *)pt, n, out);
    unhex("1ae10b594f09e26a7e902ecbd0600691", exp, 16);
    bool tag_ok = eq(out + n, exp, 16);
    ao_printf("aead tag:        %s\n", tag_ok ? "OK" : "HIBA"); fail += !tag_ok;
    bool rt = aead_decrypt(key, nonce, aad, 12, out, n + 16, buf) && eq(buf, (const u8 *)pt, n);
    ao_printf("aead roundtrip:  %s\n", rt ? "OK" : "HIBA"); fail += !rt;
    out[n + 3] ^= 1;
    bool tamper = !aead_decrypt(key, nonce, aad, 12, out, n + 16, buf);
    ao_printf("aead tamper:     %s\n", tamper ? "OK" : "HIBA"); fail += !tamper;

    /* hchacha20, draft-irtf-cfrg-xchacha 2.2.1 */
    unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, 32);
    u8 n16[16];
    unhex("000000090000004a0000000031415927", n16, 16);
    hchacha20(key, n16, out);
    unhex("82413b4227b27bfed30e42508a877d73a0f9e4d58a74a853c12ec41326d3ecdc", exp, 32);
    ao_printf("hchacha20:       %s\n", eq(out, exp, 32) ? "OK" : "HIBA"); fail += !eq(out, exp, 32);

    /* csatorna: kliens/szerver oda-vissza */
    u8 psk[32], cn[8], sn[8], hdr[12] = "AOP1\x05\x00\x01\x00\x10\x00\x00\x00";
    for (int i = 0; i < 32; i++) psk[i] = (u8)(0xA0 + i);
    for (int i = 0; i < 8; i++) { cn[i] = (u8)i; sn[i] = (u8)(0x80 + i); }
    struct aochan c, s;
    aochan_init(&c, psk, cn, sn, false);
    aochan_init(&s, psk, cn, sn, true);
    aochan_seal(&c, hdr, (const u8 *)"hello AOP titkos", 16, out);
    bool ch = aochan_open(&s, hdr, out, 32, buf) && eq(buf, (const u8 *)"hello AOP titkos", 16);
    ao_printf("csatorna:        %s\n", ch ? "OK" : "HIBA"); fail += !ch;
    bool replay = !aochan_open(&s, hdr, out, 32, buf);   /* ugyanaz a keret ujra: szamlalo mas -> hiba */
    ao_printf("replay-vedelem:  %s\n", replay ? "OK" : "HIBA"); fail += !replay;

    ao_printf("cryptotest: %s\n", fail ? "HIBA" : "minden OK");
    return fail;
}
