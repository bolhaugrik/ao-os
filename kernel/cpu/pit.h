#pragma once
#include "types.h"

#define PIT_HZ 100

void pit_init(void);
u64  pit_ticks(void);          /* 10 ms-es tickek a boot ota */
u64  pit_idle_ticks(void);     /* tickek, amikor a CPU hlt-ben volt */
void pit_sleep_ms(u32 ms);
void idle_enter(void);         /* hlt, idle-szamlalassal */
