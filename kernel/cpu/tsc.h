#pragma once
#include "types.h"

void tsc_calibrate(void);      /* PIT-hez merve, megszakitasok engedelyezve */
u64  tsc_hz(void);
u64  tsc_to_us(u64 delta);
u64  tsc_to_ms(u64 delta);
