/* Vegtelen ciklus: a cpu/deadline limit es a kill tesztjehez. */
#include "aolib.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    ao_printf("spin: pid %d, vegtelen ciklus (kill vagy hatarido allitja le)\n", ao_getpid());
    volatile u64 x = 0;
    for (;;)
        x++;
    return 0;
}
