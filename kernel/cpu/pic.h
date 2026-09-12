#pragma once
#include "types.h"

void pic_init(void);           /* atkepezes 32..47-re, minden maszkolva */
void pic_unmask(u8 irq);
void pic_mask(u8 irq);
void pic_eoi(u8 irq);
bool pic_irq_in_service(u8 irq);
