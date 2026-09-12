# AO-OS

Saját kernelű, minimális AI-terminál OS az Acer Aspire One 725 (AMD C-70) netbookra.
Terv: [docs/AO-OS-Architecture-v0.1.md](docs/AO-OS-Architecture-v0.1.md).

## Eszközök (Windows)

- LLVM (clang, ld.lld, llvm-objcopy), NASM, QEMU, Python 3

```
winget install LLVM.LLVM NASM.NASM SoftwareFreedomConservancy.QEMU
```

## Használat

```
python ao.py tools     eszközök ellenőrzése
python ao.py build     bootloader + kernel + build/ao.img
python ao.py run       QEMU ablakban, soros port a konzolon
python ao.py test      QEMU fej nélkül, soros kimenet ellenőrzése
python ao.py usb \\.\PhysicalDriveN   pendrive-ra írás (a teljes eszközt felülírja)
```

## Elrendezés

```
boot/     stage1.asm (MBR), stage2.asm (real mode: A20, E820, VBE, unreal, long mode)
kernel/   arch/ include/ lib/ drv/ kmain.c kernel.ld
tools/    mkimage.py
docs/     architektúra-terv
```

## Boot-jelzők a képernyőn

`1` stage1 · `2` stage2 · `A` A20 · `U` unreal · `M` E820 · `K` kernel betöltve · `R` ramdisk · majd grafikus mód.
Kisbetűs karakter = hiba az adott lépésnél (`a` A20, `m` E820, `d` lemez, `v` nincs VBE-mód).
