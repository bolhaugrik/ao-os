/* Minimalis JSON-olvaso a lenyomatokhoz (docs/AOP.md): kurzor-alapu, nincs fa, nincs allokacio.
 * A hivo tudja a semat: kulcsonkent dont, a tobbit jp_skip-pel atlepi. UTF-8 szoveg, \uXXXX dekodolva. */
#pragma once
#include "../kernel/include/types.h"

struct jp { const char *p, *end; };

void jp_init(struct jp *j, const char *text, usize n);
int  jp_peek(struct jp *j);                             /* a kovetkezo nem-szokoz karakter (nem fogyasztja), -1 = vege */
bool jp_expect(struct jp *j, char c);                   /* elfogyasztja, ha c kovetkezik */
bool jp_string(struct jp *j, char *out, usize cap, usize *len);   /* "..." -> out (csonkolva), len a teljes hossz cap-ig */
bool jp_number(struct jp *j, i64 *v);
bool jp_skip(struct jp *j);                             /* barmilyen ertek atlepese */
bool jp_key(struct jp *j, char *key, usize cap);        /* "kulcs": */
