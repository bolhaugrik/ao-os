#pragma once
#include "types.h"

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
bool serial_has_input(void);
u8   serial_getc(void);
