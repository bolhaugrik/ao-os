/* ChaCha20-Poly1305 AEAD (RFC 8439) + HChaCha20 kulcsszarmaztatas, freestanding C.
 * Az AOP-keretek titkositasahoz (docs/AOP.md). Nincs benne veletlenszam-forras. */
#pragma once
#include "../kernel/include/types.h"

void chacha20_block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64]);
void chacha20_xor(const u8 key[32], u32 counter, const u8 nonce[12], const u8 *in, u8 *out, usize n);
void hchacha20(const u8 key[32], const u8 nonce16[16], u8 out[32]);
void poly1305_mac(const u8 key[32], const u8 *msg, usize n, u8 tag[16]);

/* out = ciphertext || tag (n + 16 bajt) */
void aead_encrypt(const u8 key[32], const u8 nonce[12], const u8 *aad, usize aadn,
                  const u8 *pt, usize n, u8 *out);
/* in = ciphertext || tag; out = plaintext (n - 16 bajt); false, ha a tag rossz */
bool aead_decrypt(const u8 key[32], const u8 nonce[12], const u8 *aad, usize aadn,
                  const u8 *in, usize n, u8 *out);

/* AOP-csatorna: alkulcs = HChaCha20(psk, c_nonce||s_nonce), nonce = irany(4) || szamlalo(8) */
struct aochan {
    u8 key[32];
    u8 tx_dir[4], rx_dir[4];
    u64 tx_ctr, rx_ctr;
};
void aochan_init(struct aochan *c, const u8 psk[32], const u8 cnonce[8], const u8 snonce[8], bool is_server);
void aochan_seal(struct aochan *c, const u8 hdr[12], const u8 *pt, usize n, u8 *out);      /* n+16 */
bool aochan_open(struct aochan *c, const u8 hdr[12], const u8 *ct, usize n, u8 *out);      /* n-16 */
