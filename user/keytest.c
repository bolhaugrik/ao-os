/* keytest: nyers billentyu-esemenyek (lenyomas/felengedes, modositok) kiirasa; q kilep.
 * A jatekok bemeneti modjanak probaja: run keytest */
#include "aolib.h"

static const char *keyname(u16 c)
{
    switch (c) {
    case 0xE000: return "Fel"; case 0xE001: return "Le"; case 0xE002: return "Bal"; case 0xE003: return "Jobb";
    case 0xE004: return "Home"; case 0xE005: return "End"; case 0xE006: return "PgUp"; case 0xE007: return "PgDn";
    case 0xE008: return "Ins"; case 0xE009: return "Del"; case 0xE00A: return "Esc";
    case 0xE020: return "Shift"; case 0xE021: return "Ctrl"; case 0xE022: return "Alt"; case 0xE023: return "AltGr";
    case 0xE024: return "Caps";
    default: return NULL;
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (ao_con_mode(CON_RAW) != 0) { ao_puts("keytest: nincs konzol-jog\n"); return 1; }
    ao_puts("keytest: nyers mod, q kilep\n");
    int n = 0;
    for (;;) {
        struct key_ev ev[8];
        isize r = ao_read(0, ev, sizeof ev);
        if (r <= 0) break;
        bool quit = false;
        for (isize i = 0; i < r / (isize)sizeof ev[0]; i++) {
            const char *nm = keyname(ev[i].code);
            if (nm) ao_printf("  %s %s", ev[i].down ? "le " : "fel", nm);
            else if (ev[i].code >= 32 && ev[i].code < 127) ao_printf("  %s '%c'", ev[i].down ? "le " : "fel", (char)ev[i].code);
            else ao_printf("  %s U+%04x", ev[i].down ? "le " : "fel", ev[i].code);
            if (ev[i].code >= 0xE020 && ev[i].code <= 0xE024) ao_printf(" (mods=%u)", ev[i].mods);
            ao_puts("\n");
            n++;
            if (ev[i].code == 'q' && ev[i].down) quit = true;
        }
        if (quit) break;
    }
    ao_con_mode(CON_TEXT);
    ao_printf("keytest: %d esemeny\n", n);
    return 0;
}
