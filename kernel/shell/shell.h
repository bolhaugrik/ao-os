#pragma once
#include "types.h"
#include "bootinfo.h"

void shell_run(const struct bootinfo *bi);   /* nem ter vissza */
void shell_probe_bridge(void);               /* AOP PING a hidnak, allapot kiirasa (a net-taskbol) */
void shell_net_boot(bool busy);              /* a boot-kori DHCP/PING fut: az agent-inditas megvarja */

/* kprintf: konzol (+ soros tukor), a shell es a kernel kozos kimenete */
int kprintf(const char *fmt, ...);
