/* Pixel-pontos kepernyokep: F12 (vagy a 'screenshot' parancs) a framebuffert PPM-be irja a
 * /state/shots/N.ppm fajlba egy kernel-szalbol; grafikus programok (Doom) alatt is mukodik. */
#pragma once
#include "types.h"

void screenshot_init(void);         /* a VFS es a taskok utan: kernel-szal inditasa */
void screenshot_request(void);      /* IRQ-bol is hivhato: kepernyokep keresz */
