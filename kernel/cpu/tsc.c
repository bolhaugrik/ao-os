#include "tsc.h"
#include "pit.h"
#include "../arch/io.h"

static u64 hz;

void tsc_calibrate(void)
{
    u64 t0 = pit_ticks();
    while (pit_ticks() == t0)
        ;                                   /* tick-elre varunk */
    u64 start = rdtsc();
    u64 t1 = pit_ticks();
    while (pit_ticks() < t1 + 5)
        ;                                   /* 50 ms */
    u64 end = rdtsc();
    hz = (end - start) * (PIT_HZ / 5);
    if (hz == 0)
        hz = 1000000000ULL;
}

u64 tsc_hz(void) { return hz; }
u64 tsc_to_us(u64 d) { return hz ? d / (hz / 1000000) : 0; }
u64 tsc_to_ms(u64 d) { return hz ? d / (hz / 1000) : 0; }
