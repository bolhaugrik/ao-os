/* Elso ring 3-as AOX program: kiir, argumentumok, sysinfo, egy billentyut var. */
#include "aolib.h"

static int counter;   /* .bss teszt */

int main(int argc, char **argv)
{
    counter += 41;
    ao_puts("hello az AOX-bol (ring 3)!\n");
    ao_printf("pid=%d argc=%d", ao_getpid(), argc);
    for (int i = 0; i < argc; i++)
        ao_printf(" [%s]", argv[i]);
    ao_printf("  ticks=%lu  counter=%d\n", ao_ticks(), counter + 1);
    struct sysinfo si;
    if (ao_sysinfo(&si) == 0)
        ao_printf("sysinfo: %s, mem %lu/%lu MiB, %u task\n", si.version,
                  si.mem_free / (1024 * 1024), si.mem_total / (1024 * 1024), si.ntasks);
    ao_puts("nyomj egy billentyut... ");
    int k = ao_getc();
    ao_printf("(%d)\n", k);
    return 0;
}
