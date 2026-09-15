/* AO-OS: nincs libc; az alloca a fordito beepitett fuggvenye */
#pragma once
#define alloca(n) __builtin_alloca(n)
