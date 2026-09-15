/* Ctrl+V barhol: a vagolap-szal a clip-agenttel (agentd --clip, clip.cap) lekeri a PC vagolapjat a
 * hidon at, es billentyuleutesekkent adja be a futo programnak (kbd_inject, KEY_PASTE_ON/OFF kozott).
 * A masik irany (netbook -> PC) a programok dolga: agentd --file clip (shell 'copy', edit ^K). */
#pragma once
#include "types.h"

void clipboard_init(void);          /* a VFS es a taskok utan: kernel-szal inditasa */
void clipboard_request(void);       /* Ctrl+V (a billentyuzet-lekerdezesbol) */
void clipboard_request_shot(void);  /* Ctrl+F12 / Shift+F12: a kepernyo szovege a PC shots/ mappajaba (mint a 'shot') */
