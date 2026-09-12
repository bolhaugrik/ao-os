/* x86 port I/O es nehany CPU-utasitas. */
#pragma once
#include "types.h"

static inline void outb(u16 port, u8 v)  { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outw(u16 port, u16 v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outl(u16 port, u32 v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline u8  inb(u16 port)  { u8 v;  __asm__ volatile("inb %1, %0"  : "=a"(v) : "Nd"(port)); return v; }
static inline u16 inw(u16 port)  { u16 v; __asm__ volatile("inw %1, %0"  : "=a"(v) : "Nd"(port)); return v; }
static inline u32 inl(u16 port)  { u32 v; __asm__ volatile("inl %1, %0"  : "=a"(v) : "Nd"(port)); return v; }

static inline void io_wait(void) { outb(0x80, 0); }
static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }

static inline u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static inline NORETURN void halt_forever(void) {
    for (;;) { cli(); hlt(); }
}
