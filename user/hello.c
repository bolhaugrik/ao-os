/* Elso AOX program: kiir, szamol, egy billentyut var. */
#include "aolib.h"

static int counter;   /* .bss teszt */

AOX_MAIN
{
    counter += 41;
    ao->puts("hello az AOX-bol!\n");
    ao->printf("argc=%d", argc);
    for (int i = 0; i < argc; i++)
        ao->printf(" [%s]", argv[i]);
    ao->printf("  ticks=%lu  counter=%d\n", ao->ticks(), counter + 1);
    ao->puts("nyomj egy billentyut... ");
    int k = ao->getc();
    ao->printf("(%d)\n", k);
    return 0;
}
