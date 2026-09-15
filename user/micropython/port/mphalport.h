/* MicroPython AO-OS port: HAL-deklaraciok (a torzs a mphalport.c-ben) */
#pragma once
#include <stdint.h>
#include <stddef.h>

void mp_hal_set_interrupt_char(int c);
void mp_hal_init(void);

#define mp_hal_ticks_cpu mp_hal_ticks_cpu
static inline uintptr_t mp_hal_ticks_cpu(void)
{
    unsigned lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uintptr_t)hi << 32) | lo;
}

/* a VM varakozasa (utemezo): egy rovid alvas, hogy ne porgessunk */
#define MICROPY_INTERNAL_WFE(TIMEOUT_MS) mp_hal_delay_ms(1)
