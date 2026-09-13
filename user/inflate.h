/* zlib/deflate kicsomagolo (RFC 1950/1951), freestanding: a hid tomoritett kepeit bontja ki.
 * Nincs allokacio: a hivo adja a kimeneti puffert. Visszaadja a kibontott bajtok szamat vagy
 * negativ hibakodot. A zlib fejlecet (2 bajt) atugorja, az Adler-32-t nem ellenorzi. */
#pragma once
#include "../kernel/include/types.h"

isize inflate_zlib(const u8 *in, usize inlen, u8 *out, usize outcap);
isize inflate_raw(const u8 *in, usize inlen, u8 *out, usize outcap);
