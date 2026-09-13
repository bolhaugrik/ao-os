/* Szandekos page fault ring 3-ban: a kernelnek csak ezt a taskot szabad leallitania
 * (rc = E_FAULT), a gep nem allhat meg. Teszt: run fault */
#include "aolib.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    ao_puts("fault: mindjart nullat olvasok...\n");
    volatile int *p = (volatile int *)0;
    return *p;
}
