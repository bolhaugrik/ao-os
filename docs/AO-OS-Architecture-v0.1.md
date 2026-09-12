# AO-OS Architecture v0.1

Saját kernelű, minimális AI-terminál OS az Acer Aspire One (AMD C-70, ZA10 alaplap) netbookra.

Státusz: tervezési dokumentum, kód még nincs. A célhardver adatai a gépről kiolvasva (2026-09-12). Minden döntésnél három kérdésre válaszolunk: mi a döntés, miért ez kell az AI-terminál célhoz, és mit nyerünk azzal, hogy nem a hagyományos OS-utat járjuk.

---

## 0. A 20 kérdés egy oldalon

| # | Kérdés | Döntés v0.1 |
|---|--------|-------------|
| 1 | Bootstrapping | Saját kétlépcsős BIOS bootloader: 512 bájtos MBR stage1 + real-mode stage2 (INT 13h LBA, E820, VBE, A20, long mode). |
| 2 | Legacy BIOS vagy más | Legacy BIOS MBR az egyetlen bootút v0.1-ben. UEFI nincs. |
| 3 | 32 vagy 64 bit | 64 bites (long mode) kernel az első verziótól. |
| 4 | CPU-init | Bootloaderben: A20, GDT, PE, PAE+LME, paging. Kernelben: saját GDT+TSS, IDT, PIC remap, PIT, TSC, SSE-engedélyezés, NX, WP, PAT, syscall MSR-ek. Egy mag, APIC nélkül. |
| 5 | Memória 2 GB-nál (1,66 GB használható) | E820 → 4 KiB-es frame-bitmap (64 KiB). A BIOS ~340 MB-ot a GPU-nak foglal, ez az E820-ban reserved: csak a térkép szerint szabad tartományokat használjuk. Higher-half kernel, teljes fizikai memória direkt leképezése 2 MiB-es lapokkal. Nincs swap, nincs overcommit, nincs demand paging. |
| 6 | Kernelarchitektúra | Statikusan linkelt, egyetlen ring-0 image, rétegzett modulokkal ("mikro-monolit"). Nincs modulbetöltés. |
| 7 | Driver-réteg | Fordítási idejű drivertábla, init() sorrenddel. Nincs buszkeret, nincs devfs, nincs hotplug. PCI-enumeráció csak keresésre. |
| 8 | Első hardverek | PIT, PIC, i8042 billentyűzet, VBE lineáris framebuffer, COM1 (QEMU-debug), PCI, AHCI SATA (Phase 2). |
| 9 | Minimális fájlrendszer | Saját AOFS. v1: csak olvasható, host-oldalon generált image. v2: írható, blokk-bitmap + inode-tábla. |
| 10 | Saját FS vagy disk format | Phase 1: AOFS v1 image-et a bootloader tölti RAM-ba (ramdisk), nincs is disk-driver. Phase 2: AOFS v2 a SATA-n. |
| 11 | Task modell | `TASK` = saját címtér + 1 szál + capability-készlet + limitek (memória, CPU-idő, határidő). Preemptív round-robin, 100 Hz. Saját lapos AOX futtatható formátum, nincs ELF, nincs dinamikus linkelés. |
| 12 | Jogosultságok | Nincs user, uid, rwx bit. Egyetlen mechanizmus: a taskhoz kötött, csak szűkíthető capability-készlet, syscall-belépésnél ellenőrizve. |
| 13 | Agent-capability | Szöveges manifest: `fs.read`, `fs.write`, `exec`, `net`, `mem`, `cpu`, `deadline`. Glob-illesztés kanonikus útvonalon. Elutasítás naplózva, az agent visszaolvashatja. |
| 14 | AI-kommunikáció | A gépen nincs TLS/HTTP/JSON. Saját bináris AO-protokoll (AOP) egy hídhoz (bridge), amely a tényleges API-hoz beszél HTTPS-en. Titkosítás: PSK + ChaCha20-Poly1305. |
| 15 | Első agent API | `AI SERVICE` user-space task; agentek felé: `ai.open / ai.send / ai.recv / ai.close`. Tool-hívások frame-ekben, nem JSON-ban. 6 eszköz v1-ben. |
| 16 | Terminál | Framebuffer-konzol 8×16 bitmap fonttal, 170×48 karakter 1366×768-on, scrollback RAM-ban. Nem VT100, saját minimális vezérlőkészlet. |
| 17 | Állapot | A kernel állapotmentes. Minden perzisztens állapot fájl a `/state` alatt. Ugyanaz a lemez → ugyanaz a boot. |
| 18 | Image formátum | Egyetlen raw `ao.img`: MBR + stage2 + kernel-terület fix LBA-n + AOFS partíció. Ugyanaz megy QEMU-ba és USB-re. |
| 19 | QEMU | `qemu-system-x86_64 -machine pc -m 1024 -vga std -serial stdio`, raw image, soros porton automatikus smoke-teszt. |
| 20 | Valódi hardver | `ao.img` szektorpontos írása USB-re, BIOS F12 boot menü. Panic-dump képernyőre és egy fenntartott lemezszektorba. |

---

## 1. Rendszerfilozófia

**Egy gép, egy feladat, egy felhasználó.** Az AO-OS nem általános célú OS. Egy AI-agent munkakörnyezete: terminál, fájlok, feladatfuttatás, hálózat, és mindezek fölött egy kikényszerített jogosultsági modell. Minden komponens létét ez a cél indokolja.

A tervezési szűrő minden döntésnél: *erre valóban szükség van az AI-terminál célhoz?* Ha a válasz nem, kimarad. Ha a hagyományos OS tíz rétegen keresztül old meg valamit, itt egy-két célzott réteg lesz.

Amit ebből következően nem építünk:

- POSIX, libc, ELF, dinamikus linkelés, csomagkezelő
- felhasználók, csoportok, rwx bitek, setuid
- VFS-absztrakció tucatnyi fájlrendszerhez, devfs, procfs
- moduláris driver-keretrendszer, hotplug, ACPI-interpreter
- X11/Wayland, GTK/Qt, ablakkezelő
- TLS, HTTP, JSON, DNS-feloldó a gépen (ezt a híd viszi)
- swap, demand paging, overcommit
- SMP az első verziókban

Amit ehelyett kapunk: egy kicsi (Phase 1-ben becsülten 6-8 ezer soros) kódbázis, amelyet egy ember végig tud olvasni, determinisztikus boot, és egyetlen jogosultsági mechanizmus, amelyet egy függvény kényszerít ki.

---

## 2. Célhardver

Acer Aspire One, ZA10 alaplap (BIOS V2.04, Acer ZA10_BZ), ez az Aspire One 725 platform. Az adatok a gépről kiolvasva.

| Egység | Típus | Következmény a tervre |
|--------|-------|----------------------|
| CPU | AMD C-70 (Ontario, 2× Bobcat mag, 1,0 GHz, turbo 1,33 GHz), x86-64, SSE-SSSE3, NX, invariáns TSC | Long mode adott. TSC használható időmérésre, de a turbo miatt a TSC-t a PIT-hez kalibráljuk, nem a névleges órajelhez. Második mag kihagyva v0.1-ben. |
| RAM | 2 GB DDR3, E820 szerint 1741 MiB használható | A GPU UMA-területe 260 MiB a 0x6EC00000–0x7EFFFFFF tartományban, további ~30 MiB kisebb reserved/ACPI blokk. A legnagyobb összefüggő szabad blokk 1 MiB-től 0x68484000-ig (1667 MiB). Minden használható memória 4 GiB alatt van. Bitmap 64 KiB. A tényleges térkép a 4.3-ban. |
| Chipset | AMD Hudson-2 családú FCH (780x eszközök), nem A50M | SATA: 1022:7800, osztály 0106 prog-if 01 = **AHCI módban fut, a BIOS így adja át**. USB: 2× OHCI (7807) + 2× EHCI (7808) + 1× xHCI (7812). LPC-híd 780e mögött az i8042. SMBus 780b, HD Audio 780d. |
| PCI-topológia | `lspci -nm` a gépről | Host-híd 1022:1510 (Family 14h), GPU 1002:980a, két PCIe root port (1512, 1513): 01:00.0 a Realtek NIC, 02:00.0 a Broadcom WiFi. Acer alrendszer-ID 1025:0740 = Aspire One 725. |
| GPU | Radeon HD 7290 (Wrestler, "AMD Palm" az OpenGL-névben), integrált | Nincs natív driver. VBE lineáris framebuffer a BIOS-tól, ez a boot után nem változik. |
| Kijelző | 11,6", 1366×768; VBE-mód 0x1D4 = 1366×768×32 Direct Color (grub4dos `vbeprobe`) | 170×48 karakteres konzol 8×16 fonttal, 6 px jobb margóval (1366 nem osztható 8-cal). A VBE-mód sorhossza (pitch) gyakran 1368 px, ezért a rajzoló mindig a mode-info pitch-értékét használja, sosem a szélességet. |
| Billentyűzet, touchpad | i8042 a 0x60/0x64 porton, KBD IRQ 1, AUX IRQ 12; a vezérlő aktív multiplexelést (MUX 1.1) tud; a billentyűzet "AT Translated Set 2", touchpad Elantech PS/2 | Billentyűzet támogatva. A vezérlő fordítja a set 2 kódokat set 1-re, a driver ezt használja. MUX-módot nem kapcsolunk be, legacy módban hagyjuk. Touchpad kihagyva, az AUX-port letiltva. |
| Tároló | SATA-lemez az AHCI 0. portján, 6 Gbps (det=3, spd=3, sig=0x101) | **A BIOS a vezérlőt IDE-módban adja át (prog-if 0x8F).** A Linux `quirk_amd_ide_mode` mintájára a 0x40-es config-regiszter feloldásával subclass=06, prog-if=01 írható, ezután AHCI 1.3, ABAR=0x9024E000, 1 port, 64 bites DMA. A `disk` parancs ezt már megteszi, a Phase 2 AHCI-driver erre épül. |
| Ethernet | Realtek RTL810xE rev. 05 (RTL8101E/8102E/8103E család, PCI 10ec:8136) | Phase 3 HW-driver: az RTL8169-családdal azonos regiszterkészlet, leíró-gyűrűs DMA, jól dokumentált. QEMU-ban e1000 marad, mert a QEMU rtl8139-e más chip. |
| WiFi | Broadcom BCM4313 802.11n | Kihagyva. Zárt firmware-t igényel, driver több ezer sor. Vezetékes Ethernet. |
| BIOS | Insyde, V2.04, **legacy BIOS-boot megerősítve** (nincs `/sys/firmware/efi`), USB-boot működik | A tervezett MBR-bootút biztosan járható. ACPI RSDP a 0xFE020 címen (a 0xE0000–0xFFFFF keresés megtalálja). EBDA a 0x9F400-tól: a stage2 minden alacsony puffere ez alatt marad. |
| Egyéb MMIO | PCIe MMCONFIG 0xF8000000, IOAPIC 0xFEC00000, LAPIC 0xFEE00000, FCH 0xFED80000, xHCI USB 3.0 jelen | v0.1-ben egyik sem kell (PIC + 0xCF8 config-port). A címek a Phase 4 SMP/APIC munkához rögzítve. |
| Soros port | nincs | Valódi HW-n a képernyő és a lemez a debug-csatorna. |

---

## 3. Boot stratégia

**Döntés:** saját kétlépcsős legacy BIOS bootloader.

```text
BIOS  →  stage1 (MBR, 512 B, asm)  →  stage2 (~24 KiB, real mode, asm)  →  kernel (long mode, C)
```

**stage1** (LBA 0): partíciós táblát tartalmaz, hogy a BIOS és a USB-HDD emuláció elfogadja. Egyetlen dolga: INT 13h/42h-val beolvassa a stage2-t az LBA 1..63 tartományból a 0x8000 címre és ráugrik. Hibánál egy karaktert ír és megáll.

**stage2** sorrendben:

1. A20 engedélyezés (BIOS INT 15h/2401h, tartalék: 0x92-es port).
2. E820 memóriatérkép lekérése egy fix pufferbe (0x9000).
3. VBE: módlista lekérése (4F00h/4F01h), kiválasztás: 1366×768×32 (a panel natív módja), ha nincs, 1280×720×32, majd 1024×768×32, végül 800×600×32. A módválasztó a mode-info szélesség/magasság/bpp mezőit nézi, nem fix módszámot, mert a 1366×768 módszáma BIOS-onként eltér. Ezen a gépen a grub4dos `vbeprobe` szerint a mód **0x1D4, 1366×768×32, Direct Color**, tehát az első lépcső valódi HW-n talál; QEMU-ban a második. Módváltás 4F02h-val, lineáris framebuffer bittel. A `bootinfo`-ba a pitch (bájt/sor) is bekerül. Ha egyik mód sincs, hibaüzenet 80×25 szöveges módban és megállás.
4. Unreal mode bekapcsolása, a kernel beolvasása INT 13h-val közvetlenül az 1 MiB fizikai címre (LBA 64-től, fix szektorszám a kernel fejlécéből).
5. Az AOFS v1 ramdisk-image beolvasása a 4 MiB fizikai címre (Phase 1-ben ez helyettesíti a lemez-drivert).
6. Kezdeti laptáblák felépítése (identity 0..1 GiB 2 MiB-es lapokkal + higher-half leképezés), GDT, CR4.PAE, EFER.LME, CR0.PG → long mode.
7. Ugrás a kernel belépési pontjára, RDI-ben a `bootinfo` struktúra fizikai címe (E820, framebuffer paraméterek, kernel és ramdisk helye, TSC-érték a boot elején).

**Miért nem GRUB/Multiboot?** Három ok. A VBE-módváltást úgyis real modeban kell elvégezni, tehát a real-mode kód nem spórolható meg. A teljes bootút a mi kódunk, így determinisztikus és hibakereshető. A GRUB önmagában nagyobb, mint a Phase 1 kernel.

**Miért nem UEFI?** A célgép BIOS-a legacy. Az UEFI-út egy második bootloadert, PE-formátumot és GOP-kezelést jelentene ugyanazért az eredményért. Ha később kell, a `bootinfo` struktúra bootloader-független, egy UEFI stage beilleszthető a kernel módosítása nélkül.

**Mit nyerünk:** egyetlen bootút, 1 másodperc alatti BIOS→kernel átadás, nulla külső bináris a lemezen.

---

## 4. CPU és memória stratégia

### 4.1 Miért 64 bit az első verziótól

- A C-50 x86-64. A 32 bites kernel egy kisebb címtér és a PAE/nem-PAE dilemma mesterséges korlátját hozná be.
- Long modeban a szegmentálás gyakorlatilag eltűnik, a laptábla egyetlen formátumú (4 szintű), a NX bit alapból elérhető.
- 16 általános regiszter, SSE2 garantált: a fordító jobb kódot generál, kevesebb assembly kell.
- Egyetlen ABI, egyetlen fordítási cél (`x86_64-elf`). Nincs 32 bites kompatibilitási mód, nincs kétféle user-space.
- Ára: a stage2-ben a 16→32→64 átmenet néhány száz sor assembly, amit egyszer kell megírni.

### 4.2 CPU-inicializálás a kernelben

| Lépés | Mit csinál | Miért kell |
|-------|-----------|------------|
| GDT + TSS | null, kcode64, kdata, ucode64, udata, TSS (IST1 a double fault-nak) | Ring 3 taskokhoz és a kernelverem-váltáshoz. |
| IDT | 256 bejegyzés, 32 kivétel + 16 IRQ + syscall-vektor nélkül (syscall MSR-ből megy) | Kivételkezelés az első pillanattól: minden hiba olvasható dumpot ad. |
| PIC remap | 8259A IRQ 0-15 → 32-47 vektorok | Egyszerűbb, mint az APIC, egy magnak elég. |
| PIT | 100 Hz, IRQ0 | Scheduler-tick, időzítők, határidők. |
| TSC | kalibrálás a PIT-hez képest boot alatt | Mikroszekundumos mérőszámok (boot idő, shell-latencia). |
| CR0.WP, EFER.NXE | írásvédelem kernel módban, no-execute lapok | Kernel-kódot nem ír felül hiba, adat-lap nem futtatható. |
| CR4.OSFXSR, OSXMMEXCPT | SSE engedélyezése | User taskok használhatják. A kernel `-mno-sse`-vel fordul, nem használja. |
| PAT | egy PAT-bejegyzés write-combining módra | A framebuffer WC-vel leképezve; enélkül a képernyőírás valódi HW-n nagyságrenddel lassabb. |
| EFER.SCE, STAR, LSTAR, SFMASK | `syscall`/`sysret` | A leggyorsabb ring-váltás, `int 0x80` helyett. |
| Idle | `hlt` a scheduler üres ágán | Idle CPU nulla közelében, hűvös gép. |

Nincs: APIC, IOAPIC, SMP, ACPI-interpreter (kivéve a kikapcsoláshoz szükséges FADT-olvasás Phase 2-ben), mikrokód, MTRR-hangolás a PAT-on túl.

### 4.3 Memória

**Fizikai:** E820-ból bitmap-allokátor, 4 KiB frame-ek, 64 KiB bitmap a 2 GB-os fizikai címtartományra. Az első 1 MiB, a kernel, a ramdisk, a laptáblák és minden E820-ban nem "usable" tartomány foglaltként indul. A direkt leképezés a legmagasabb E820-címig tart, nem 2 GB-ig fixen.

A gép tényleges E820-térképe (Puppy Linux `dmesg`-ből):

```text
0x00000000 - 0x0009F3FF  usable      637 KiB   (stage1/stage2, alacsony pufferek)
0x0009F400 - 0x0009FFFF  reserved              EBDA
0x000E0000 - 0x000FFFFF  reserved              BIOS ROM, RSDP a 0xFE020-nál
0x00100000 - 0x68483FFF  usable     1667 MiB   kernel 1 MiB-től, ramdisk 4 MiB-től, minden allokáció
0x68484000 - 0x68C84FFF  reserved      8 MiB
0x68C85000 - 0x68CBBFFF  usable      220 KiB
0x68CBC000 - 0x6A0BBFFF  reserved     20 MiB
0x6A0BC000 - 0x6E9BEFFF  usable       73 MiB
0x6E9BF000 - 0x6EABEFFF  reserved      1 MiB
0x6EABF000 - 0x6EBBEFFF  ACPI NVS      1 MiB
0x6EBBF000 - 0x6EBFEFFF  ACPI data   256 KiB   (FADT itt, a kikapcsoláshoz)
0x6EBFF000 - 0x6EBFFFFF  usable        4 KiB
0x6EC00000 - 0x7EFFFFFF  reserved    260 MiB   GPU UMA (a BIOS foglalja a Radeonnak)
0xF8000000 - 0xFBFFFFFF  reserved              PCIe MMCONFIG
0xFEC00000 / 0xFEE00000  reserved              IOAPIC / LAPIC
0xFFC00000 - 0xFFFFFFFF  reserved              BIOS flash
összesen használható:               1741 MiB
```

Következmények a tervre:

- A kernel, a ramdisk és a laptáblák mind az 1667 MiB-os fő blokkba kerülnek, a PMM-nek nem kell töredezett alacsony tartományokkal indulnia.
- Nincs használható memória 4 GiB felett, ezért a direkt leképezés 2 GiB-nyi 2 MiB-es lap, egyetlen PDPT-vel. Minden DMA-puffer automatikusan 32 bites címen van.
- A 4 KiB-nál kisebb "usable" szigeteket (pl. 0x6EBFF000) a PMM egyszerűen kihagyja, nem ér meg egy külön esetet.
- A stage2 minden alacsony memóriás puffere (E820-tábla, VBE mode-info, verem) 0x9F400 alatt marad. Egy összefüggő tartomány-allokátor (`pmm_alloc_contig`) a DMA-pufferekhez (AHCI, NIC).

**Virtuális címtér:**

```text
0x0000_0000_0000_0000 .. 0x0000_7FFF_FFFF_FFFF   user (taskonként külön PML4)
    0x0000_0000_0040_0000  AOX kód+adat, fix betöltési cím
    0x0000_7FFF_F000_0000  user verem (lefelé nő)
0xFFFF_8000_0000_0000 ..                         direkt leképezés: teljes fizikai memória, 2 MiB-es lapok
0xFFFF_FFFF_8000_0000 ..                         kernel kód/adat (higher half, -mcmodel=kernel)
0xFFFF_FFFF_C000_0000 ..                         kernel heap
0xFFFF_FFFF_E000_0000 ..                         framebuffer (WC)
```

A felső fél minden PML4-ben azonos (megosztott laptábla-ág), ezért a címtérváltás olcsó és a kernel bármely fizikai címet elér a direkt leképezésen át laptábla-piszkálás nélkül.

**Kernel heap:** méretosztályos free-list allokátor (16 B … 4 KiB), nagyobb kérés közvetlenül frame-ekből. Nincs slab-keretrendszer, nincs cache-színezés.

**Szabályok:** eager allokáció (task indításakor minden lapja fizikailag ott van), nincs swap, nincs overcommit, nincs copy-on-write. Ha nincs elég memória, a `task.spawn` azonnal hibával tér vissza, nem később page-fault-tal.

**Mit nyerünk:** determinisztikus memória-viselkedés, nincs OOM-killer, nincs lazy-fault útvonal, amelyet debugolni kellene. 1741 MiB használható memória mellett ez a luxus megengedhető.

---

## 5. A kernel felépítése

**Döntés:** egyetlen statikusan linkelt ring-0 image, belül szigorúan rétegzett, fordítási időben rögzített modulokkal. Nincs betölthető modul, nincs kernel-belső dinamikus regisztráció.

```text
kernel/
  arch/     entry.asm, gdt, idt, isr-stubok, context switch, syscall entry, msr, io
  mm/       pmm (bitmap), vmm (laptáblák), kheap
  cpu/      exceptions, irq, pit, tsc, panic
  drv/      fbcon, font, kbd, serial, pci, ahci (P2), e1000/atl1c (P3)
  fs/       ns (névtér + útvonal-kanonizálás), aofs, ramfs (/sys, /tmp)
  task/     task, sched, aox loader, syscalls, pipe
  cap/      cap_check, manifest parser, audit ring
  net/      (P3) arp, ipv4, udp, dhcp, tcp, aop
  shell/    line editor, parancsok
  lib/      memcpy/memset/strlen/fmt (saját, ~300 sor)
```

Függőségi irány felülről lefelé: shell → task/cap/fs → drv → mm → arch. Alsó réteg sosem hív felsőt.

**Miért nem mikrokernel?** Az IPC-alapú izoláció akkor fizet, ha egymástól védeni kell driverek és szolgáltatások sokaságát. Itt öt driver van, mind a mi kódunk, egy felhasználóval. Az izolációt, ami tényleg számít (agent vs. rendszer), a task-capability modell adja ring 3-ban.

**Miért nem hagyományos monolit?** Nincs modulbetöltés, nincs runtime driver-regisztráció, nincs általános eszközmodell. A drivertábla egy statikus tömb; ami nincs benne, az nem létezik.

**Shell helye:** Phase 1-ben a shell kernel-szál (ring 0), mert így a mérföldkő syscall-réteg nélkül elérhető. A shell azonban a kezdettől egy "syscall alakú" belső API-t hív (`sys_read`, `sys_list`, `sys_spawn`…), ezért Phase 2-ben a ring 3-ba költöztetés mechanikus: ugyanazok a hívások, csak a `syscall` utasításon keresztül.

---

## 6. Driver stratégia

**Döntés:** minden driver egy fix `struct` néhány függvénymutatóval, egy statikus init-tábla sorrendjében indul.

```text
console:  putc, write, clear, set_cursor
input:    poll_key, wait_key
block:    read_sectors, write_sectors, sector_count
netdev:   send, recv, mac
```

Négy interfész, nem több. Nincs `ioctl`, nincs eszközfájl, nincs referenciaszámlálás.

**Első körben támogatott hardver és sorrend:**

| Phase | Driver | Sorok (becslés) | Indok |
|-------|--------|----------------|-------|
| 0 | serial COM1 | 60 | QEMU-ban a soros port a teszt-kimenet. Valódi HW-n nincs, a driver ártalmatlan. |
| 1 | fbcon + 8×16 font | 400 | Az egyetlen kijelző-út. |
| 1 | i8042 billentyűzet, scancode set 1 | 250 | PS/2 belső billentyűzet, a vezérlő fordít set 1-re. Init: puffer-flush, önteszt, AUX-port letiltása, MUX nem engedélyezve, IRQ 1. US és HU kiosztás táblából. |
| 1 | PIT, PIC | 120 | Tick és megszakítás. |
| 1 | PCI config (0xCF8/0xCFC) | 150 | AHCI és NIC megtalálása. Csak enumeráció, nincs erőforrás-allokáció. |
| 2 | AHCI SATA | 700 | Írható tároló valódi HW-n. BIOS-t AHCI módra állítjuk. |
| 3 | e1000 (QEMU) | 600 | Hálózati protokoll fejlesztése emulátorban. |
| 3 | RTL8101E-család (10ec:8136, HW) | 700 | A netbook valódi NIC-je. RTL8169-stílusú regiszterek, egy RX és egy TX leíró-gyűrű, 100 Mbit. |

Tudatosan kihagyva: USB (OHCI/EHCI + HID + MSC együtt 3-4 ezer sor lenne), WiFi, hang, touchpad, ACPI-interpreter, Radeon natív driver, HPET.

---

## 7. Storage stratégia

**Döntés:** saját AOFS, két lépcsőben.

**AOFS v1 (Phase 1, csak olvasás, ramdisk):**

```text
superblock (4 KiB):  magic "AOFS", verzió, bejegyzésszám
entry-tábla:         { név[64], típus, offset, méret, flags }  – lapos névtér, "/" a nevekben
adatterület
```

A host-oldali `mkaofs.py` építi a `rootfs/` könyvtárból. A bootloader tölti RAM-ba, így a Phase 1 mérföldkő (`ls`, `cat`, `run`) lemez-driver nélkül elérhető. Ez a döntés szedi ki a legnagyobb kockázatot (AHCI valódi HW-n) az első mérföldkőből.

**AOFS v2 (Phase 2, írható, SATA):**

- 4 KiB blokk, blokk-bitmap, fix méretű inode-tábla, extent-lista inode-onként (max 8 extent, utána indirekt blokk).
- Könyvtárak: inode-lista, névvel. Nincs symlink, nincs hardlink, nincs jogosultsági bit az inode-ban.
- Írási sorrend: adat → inode → bitmap → superblock `dirty` flag törlése. Boot-kor `dirty` esetén gyors ellenőrzés (bitmap újraszámolása az inode-okból). Nincs journal.
- Mount-tábla legfeljebb 4 bejegyzéssel: `/` (AOFS), `/sys` (kernel-állapot, ramfs, csak olvasás), `/tmp` (ramfs), `/state` (az AOFS egy könyvtára, csak névkonvenció).

**Miért nem FAT32?** A FAT-tal a host közvetlenül tudná írni a lemezt, de LFN, két FAT, cluster-lánc, és semmilyen attribútum a mi céljainkhoz. A fájlcsere a host és a gép között a `mkaofs` image-generálással, később hálózaton történik. Egy csak-olvasó FAT-olvasó (USB-stick adatcsere) Phase 3 után opcionális.

**Miért nincs symlink?** A capability-illesztés kanonikus útvonalon történik. Symlink nélkül nincs útvonal-alias, tehát nincs TOCTOU és nincs kiszökés a `/project/**` alól.

---

## 8. Task/process modell

**Döntés:** a `TASK` a rendszer egyetlen futási egysége.

```text
TASK {
  id, parent, state
  pml4, kernel_stack, user_stack
  regs (context)
  caps[]                 – capability-készlet, létrehozáskor rögzül
  cwd
  handles[16]            – fájl, pipe, ai-session
  mem_limit, mem_used
  cpu_budget_ms, deadline_tick
  exit_code
}
```

- Egy task = egy címtér = egy szál. Párhuzamosság több taskkal, nem szálakkal. Az agentnek nem szálak kellenek, hanem izolált, határidős eszközfuttatások.
- Ütemezés: preemptív round-robin, 100 Hz, két prioritási sor (interaktív: shell, aisvc; háttér: agent-eszközök). Nincs nice, nincs CFS.
- Futtatható formátum **AOX**: 32 bájtos fejléc (magic, kód-, adat-, bss-méret, belépési pont, verem-méret) + lapos kép, fix 0x400000 címre. A host-linker-script állítja elő. Nincs ELF, nincs relokáció, nincs dinamikus linkelés.
- Syscall-készlet célzottan ~24 hívás: `exit, spawn, wait, kill, yield, sleep, read, write, open, close, list, stat, mkdir, unlink, pipe, getcaps, sysinfo, ai_open, ai_send, ai_recv, ai_close, net_connect, net_send, net_recv`.
- IPC: bájtfolyam-pipe (4 KiB gyűrűpuffer). Ez elég a tool-kimenet elkapásához.
- Limitek kikényszerítése: `mem_limit` a lap-allokációnál, `cpu_budget_ms` a ticknél, `deadline_tick` a ticknél → a task `KILLED_TIMEOUT` állapotba kerül, a szülő `wait` hívása ezt visszakapja.

**Mit nyerünk:** az agent minden eszközfuttatása egy határidős, memóriakorlátos, capability-szűkített task. Nem kell cgroup, ulimit, seccomp, namespace: egy struktúra hat mezője.

---

## 9. Permission / capability modell

**Döntés:** nincs felhasználó, nincs tulajdonos, nincs jogosultsági bit a fájlokon. Egyetlen kérdés létezik: *ez a task birtokolja-e ezt a capability-t?*

```text
cap := { kind, pattern, limit }
kind  ∈ { fs.read, fs.write, fs.exec, exec, net, task.spawn, sys.info, sys.reboot, sys.poweroff, dev.console }
```

**Szabályok**

1. A gyökér-task (init/shell) minden capability-t birtokol.
2. Egy gyerek-task csak a szülő készletének részhalmazát kaphatja. A kernel a `spawn`-nál ellenőrzi: minden kért cap-nek illeszkednie kell egy szülő-capre (a minta a szülő mintájának szűkítése).
3. A készlet a task élete alatt nem bővíthető.
4. Minden syscall, amely erőforrást nevez meg, belépéskor egyetlen függvényt hív: `cap_check(task, kind, resource)`. Elutasításnál `E_CAP`, és a hívás naplózódik egy gyűrűpufferbe (`/sys/audit`).
5. Útvonalakat a kernel a `cwd`-vel összefűzve kanonizál (`.`/`..` feloldva) az illesztés előtt. Symlink nincs, tehát a kanonikus forma egyértelmű.
6. Glob: `*` egy komponensen belül, `**` tetszőleges mélységben. Csak ennyi.
7. `net` minta: `host[:port]`, a `net_connect` hívásnál a host-nevet a híd oldja fel, a kernel a manifestben szereplő szöveges nevet és portot illeszti.

**Miért nem Linux-jogosultságok?** Az rwx/uid modell a "ki" kérdésre válaszol, nekünk a "mit" kérdés kell: ez az agent-futás a `/project/src/**` alá írhat-e. Az ACL-ek és a mandatory access control ugyanezt tíz réteggel teszik. Itt egy 150 soros függvény és egy manifest-parser.

---

## 10. Terminál

**Döntés:** saját framebuffer-konzol, nem VT100-emuláció.

- 8×16 bitmap font (saját, ISO-8859-2 lefedéssel az ékezetekhez), 1366×768-on 170×48 cella, a jobb szélen 6 px kihasználatlan sáv.
- Scrollback: 2000 sor × 170 cella × 2 bájt (karakter + attribútum) = 680 KiB RAM-ban. Shift+PgUp/PgDn lapoz.
- Vezérlőkészlet: `\n`, `\r`, `\b`, `\t`, törlés, kurzor-pozíció, 16 szín előtér/háttér. ANSI SGR-ből csak a szín-részhalmaz (`ESC[3xm`, `ESC[0m`), mert az AI-kimenet ezt gyakran tartalmazza. Semmi más.
- Rajzolás: csak a megváltozott cellák (dirty-flag cellánként), a framebuffer WC-leképezésen. Görgetésnél a scrollback-ből újrarajzolás, nem `memmove` a VRAM-ban.
- Soros tükör: minden konzol-kimenet a COM1-re is megy, ha jelen van. QEMU-ban ez a tesztek kimenete.
- Line editor a shellben: kurzor bal/jobb, Home/End, előzmény (fel/le, 64 bejegyzés), Ctrl+C → az előtér-task `kill`, Ctrl+L → clear.
- Billentyűzet: scancode set 1 → keycode → karakter, kiosztás táblából (`us`, `hu`), a táblát `/state/kbd` választja.

Nincs: több virtuális terminál, ablakok, egérkurzor, Unicode-shaping. Ha Phase 3-ban kell egy "agent-napló" nézet a shell mellett, az a konzol kettéosztásával (két cella-régió) oldható meg, nem ablakkezelővel.

---

## 11. AI service interfész

**Döntés:** a netbook nem beszél HTTPS-t. Egy **híd** (bridge) beszél helyette.

```text
[AO-OS]  aisvc task  ──AOP/TCP (PSK+ChaCha20-Poly1305)──▶  [bridge: LAN-PC, VPS vagy saját szerver]  ──HTTPS/JSON──▶  AI-szolgáltató
```

**Miért?** Egy TLS 1.3-kliens X.509-ellenőrzéssel, HTTP/1.1-kliens és JSON-parser együtt 6-10 ezer sor lenne, több mint a Phase 1 kernel, és folyamatos karbantartást igényel (tanúsítványok, ciphersuite-ok). A híd egy 300 soros Python-program bárhol, ahol van internet. A gép így szolgáltató-független: a híd cseréli az API-t, az OS nem változik.

**AOP (AO Protocol) v1**

```text
frame: u32 magic 'AOP1' | u16 type | u16 flags | u32 len | payload[len]
type:  HELLO, AUTH, PROMPT, DELTA, TOOL_CALL, TOOL_RESULT, END, ERR, PING
```

- Egy TCP-kapcsolat, keretezett üzenetek, nincs újracsatlakozás-logika a kernelben (az `aisvc` task feladata).
- Titkosítás: előre megosztott kulcs a `/state/ai/psk` fájlban, ChaCha20-Poly1305 keretenként, számláló-nonce. Ez ~500 sor freestanding C, tanúsítvány-infrastruktúra nélkül. Phase 3.0 titkosítás nélkül LAN-on, 3.1-ben titkosítva.
- A `TOOL_CALL` payload nem JSON: `név\n` + `kulcs=érték\n` sorok. A híd fordítja a szolgáltató tool-call formátumát erre és vissza.

**AI SERVICE = `aisvc` user-task.** Egyedül ő birtokolja a `net bridge-host:port` capability-t. Az agentek felé a kernel `ai_open/ai_send/ai_recv/ai_close` syscalljai egy pipe-párt adnak az `aisvc`-hez. Az `aisvc` csere esetén (másik híd, később lokális modell) az agentek és a kernel nem változnak.

---

## 12. Agent modell

**AGENT = TASK + manifest + AI-session.**

Manifest (`/state/agents/<név>.cap`, szöveges):

```text
agent coder
  fs.read    /project/**
  fs.write   /project/src/**
  fs.read    /state/agents/coder/**
  fs.write   /state/agents/coder/**
  exec       build
  exec       test
  mem        64M
  cpu        30s
  deadline   120s
```

Az `exec build` egy nevesített parancs: a `/sys/tools/build` bejegyzés egy AOX-fájlra és egy rögzített argumentumlistára mutat. Az agent nem futtathat tetszőleges binárist, csak nevesített eszközöket.

**Agent-futásciklus (`agentd`, Phase 3):**

```text
1. manifest betöltése → caps
2. ai_open(profil)
3. kontextus küldése: feladat + /state/agents/<név>/context
4. ai_recv: DELTA-k a konzolra, TOOL_CALL → 
     fs.read / fs.write / fs.list / task.run(név, timeout) / ask_user / done
   végrehajtás a saját capjeivel (a kernel kényszeríti ki)
5. TOOL_RESULT vissza, ugrás 4-re, amíg END
6. kontextus mentése fájlba, exit
```

Első tool-készlet: `fs.read`, `fs.write`, `fs.list`, `task.run`, `ask_user`, `done`. Hat eszköz, több nem. Minden tool egy-egy syscall-ra képződik le, tehát minden eszközhívás átmegy a `cap_check`-en. A híd és a szolgáltató nem tud olyat kérni, amit a manifest nem enged.

**Visszacsatolás:** az `E_CAP` elutasítások a `/sys/audit`-ban látszanak, a `caps` shell-parancs és az agent `TOOL_RESULT`-ja is visszaadja. Az agent így tudja, hogy nem hiba, hanem jogosultság hiányzik.

---

## 13. Build toolchain

Fejlesztő host: Windows 11. Cél: nulla natív cross-compiler build.

| Eszköz | Választás | Miért |
|--------|-----------|-------|
| C-fordító | `clang --target=x86_64-elf` (LLVM) | Beépített cross-target, nem kell binutils/gcc fordítás Windows-on. |
| Linker | `ld.lld` linker-scriptekkel | Az LLVM része, kezeli a `-mcmodel=kernel` higher-half elrendezést. |
| Assembler | NASM | stage1/stage2 és a kernel asm-fájljai. Olvasható 16 bites kód. |
| Build-vezérlő | `ao.py` (Python 3, csomag nélkül) | `build`, `image`, `run`, `debug`, `test`, `usb` alparancsok. Make helyett, mert Windows-on ez a legkevesebb súrlódás. |
| Host-eszközök | `tools/mkaofs.py`, `tools/mkimage.py`, `tools/bridge.py` (P3) | Image-építés és a híd. |
| Emulátor | QEMU for Windows | lásd 14. |
| Debugger | `gdb` (MSYS2 vagy WSL) a QEMU gdbstubhoz | Opcionális, a soros-port-nyomkövetés a fő eszköz. |

Fordítási flag-ek a kernelre:

```text
-ffreestanding -fno-builtin -nostdlib -nostdinc -fno-stack-protector -fno-pic
-mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel
-O2 -g -Wall -Wextra -Werror -std=c11
```

A `-mno-red-zone` kötelező: megszakítás a kernelveremre érkezik, a red zone felülíródna. A kernel semmilyen libc-t nem lát (`-nostdinc`), a `lib/` saját `memcpy/memset/strlen/fmt` implementációt ad, mert a fordító ezeket generált kódból is hívhatja.

Repo-elrendezés:

```text
ao-os/
  boot/     stage1.asm  stage2.asm
  kernel/   (5. fejezet szerint)  kernel.ld
  user/     crt0.asm  aolib/  tools/   (P2+)
  rootfs/   a ramdisk / AOFS tartalma
  tools/    mkaofs.py  mkimage.py  bridge.py
  tests/    smoke.py  (soros porton keresztül)
  docs/
  ao.py
```

---

## 14. QEMU tesztkörnyezet

```text
qemu-system-x86_64 -machine pc -m 1024 -cpu Opteron_G3
  -drive file=build/ao.img,format=raw,if=none,id=d0
  -device ahci,id=ahci -device ide-hd,drive=d0,bus=ahci.0
  -vga std -serial stdio -no-reboot -no-shutdown
  [-netdev user,id=n0,hostfwd=tcp::9010-:9010 -device e1000,netdev=n0]   (P3)
  [-s -S]  gdb
  [-d int,cpu_reset]  kivétel-nyomkövetés
```

- `-vga std` a Bochs VBE-t adja, a stage2 ugyanazt a VBE-utat járja, mint valódi HW-n. A QEMU módlistájában nincs 1366×768, ott a választó 1280×720×32-re esik vissza: ez szándékosan teszteli a fallback-ágat és a pitch-alapú rajzolást.
- Az AHCI-t az `-device ahci` adja a `pc` gépen; Phase 1-ben (ramdisk) lemez-driver nélkül is fut.
- Windows-on WHPX gyorsítás (`-accel whpx`) opcionális; a TCG is bőven elég 1 GHz-es célnál.
- **Automatikus smoke-teszt** (`tests/smoke.py`): elindítja a QEMU-t, a soros porton várja az `AO>` promptot, beküldi a `help`, `mem`, `cpu`, `ls` parancsokat, ellenőrzi a kimenetet, méri a boot-időt a soros időbélyegekből. Ez minden build után fut.
- Panic-teszt: egy `crash` shell-parancs szándékosan kivételt vált ki, a teszt a regiszter-dump formátumát ellenőrzi.

---

## 15. Valódi hardveres tesztelés

**Telepítés USB-re:** az `ao.py usb \\.\PhysicalDriveN` szektorpontosan írja az `ao.img`-t (Win32DiskImager vagy Rufus DD-mód is jó). A BIOS-ban: F12 boot menü → USB HDD; SATA mód: AHCI.

**Első HW-lépések, ebben a sorrendben:**

1. stage1 egy karaktert ír (`1`), stage2 másikat (`2`): a bootút látható a képernyőn, akkor is, ha a VBE-váltás elszáll.
2. VBE-módlista kiírása szöveges módban egy `vbe-list` boot-flaggel, mielőtt a módváltást élesítjük.
3. Kernel első kiírása framebufferre; `mem`, `cpu` parancsok.
4. Billentyűzet (i8042 MUX-képes vezérlő: kezdeti flush, AUX letiltás, a translate-bit már be van kapcsolva a BIOS által, ezt megtartjuk).
5. Phase 2: AHCI-port enumeráció, `disk` parancs; olvasás; írás csak külön partícióra.

**Debug soros port nélkül:**

- Panic-dump a képernyőre: kivétel-szám, hibakód, RIP, RSP, CR2, regiszterek, 8 visszaugró verem-cím. Egy fénykép elég.
- **Panic-store:** a dump 512 bájtos formában a lemez egy fenntartott szektorába (`PANIC` régió, LBA 2048) is kiíródik, boot-kor a `lastpanic` parancs kiolvassa. Ehhez Phase 1-ben a BIOS INT 13h nem használható már (long mode), ezért ez Phase 2-ben, az AHCI-írással jön.
- Boot-fázis jelzők a képernyő jobb felső sarkában (fázisszám), így a lefagyás helye látszik.

**Ismert HW-kockázatok:** a VBE-módlista eltérhet a QEMU-tól (ezért a 4 lépcsős fallback); a 1366×768 mód pitch-e lehet 1368 px, a rajzoló ezt kezeli; az EC-s i8042 lassabb, mint az emulált; az AHCI BIOS-átadáskor lehet, hogy a BIOS "owner" bit átvételét (BIOS/OS handoff) kéri; az RTL8101E driver a legnagyobb HW-specifikus munka.

---

## 16. Mérőszámok

Minden mérőszámhoz mérési módszer tartozik, és a `bench` shell-parancs kiírja őket.

| Mérőszám | Phase 1 cél | Phase 3 cél | Mérés |
|----------|-------------|-------------|-------|
| `ao.img` mérete | < 2 MiB | < 6 MiB | fájlméret |
| kernel bináris | < 200 KiB | < 512 KiB | linker-kimenet |
| BIOS→`AO>` prompt | QEMU < 0,5 s, HW < 2 s | HW < 3 s | TSC a stage2 elején (bootinfo) és a prompt kiírásakor |
| RAM a prompt után (kernel + heap + ramdisk nélkül) | < 4 MiB | < 12 MiB | `mem` parancs, pmm-számlálók |
| billentyű→echo latencia | < 2 ms | < 2 ms | TSC az IRQ1-ben és a cella-rajzoláskor |
| idle CPU | ~0 % (`hlt`) | ~0 % | tick-számláló: idle-tickek aránya |
| kódsorok (asm+C, teszt nélkül) | < 8 000 | < 25 000 | `ao.py loc` |
| task spawn+exit | – | < 1 ms | TSC a syscall-ban |
| AI round-trip a hídig (LAN) | – | < 20 ms hálózati overhead | AOP PING |

Ha egy változtatás bármelyik mérőszámot rontja, indokolni kell a commitban. Nem optimalizálunk mérés nélkül.

---

## 17. Amit tudatosan kihagyunk, és mikor kerülhet vissza

| Kihagyva | Miért | Visszakerülhet |
|----------|-------|----------------|
| SMP (2. mag) | Determinizmus, egyszerűbb scheduler; az AI-terminál nem CPU-kötött | Phase 4, ha egy lokális modell futtatása igényli |
| USB | 3-4 ezer sor, PS/2 billentyűzet és SATA fedi az igényt | Phase 4, USB-Ethernet vagy stick-adatcsere miatt |
| TLS a gépen | lásd 11. | Ha a híd nélküli üzem cél lesz |
| ELF, dinamikus linkelés | Saját AOX, statikus | Soha |
| Felhasználók, jogosultsági bitek | Capability-modell fedi | Soha |
| WiFi | Driver mérete és firmware-függés | Nem tervezett |
| ACPI-interpreter | Csak a kikapcsoláshoz kell FADT/PM1a, az 200 sor | Nem tervezett |
| Lokális LLM | 1,66 GB RAM mellett csak apró (≤1 GB-os, kvantált) modell | Phase 4, kísérlet |

---

## 18. Fejlesztési terv

### Phase 0 — Boot és toolchain (cél: a kernel egy sort ír ki QEMU-ban és a netbookon)

1. Repo-elrendezés, `ao.py build/image/run`, clang+lld+nasm beállítása, `mkimage.py`.
2. stage1: MBR, partíciós tábla, stage2 betöltése.
3. stage2: A20, E820, VBE-módválasztás, unreal mode, kernel betöltés 1 MiB-re, laptáblák, long mode, `bootinfo`.
4. Kernel `entry.asm` + `kmain()`: soros port, framebuffer-teszt (kitöltés + egy karakter), majd `hlt`.
5. USB-teszt a netbookon: `1`, `2`, framebuffer-kitöltés látszik.

Kilépési feltétel: ugyanaz az `ao.img` bootol QEMU-ban és a netbookon, fénykép a képernyőről.

**Teljesítve 2026-09-12.** QEMU: soros smoke-teszt zöld (`ao.py test`). Netbook: USB-ről indítva azonnal a natív 1366×768×32 mód (VBE 0x1D4), AO-OS felirat, prompt, helyes RGB-sorrend. Mért értékek: kernel.bin 9,3 KB, stage2.bin 1,2 KB, ao.img 8 MiB. Egy hiba került elő és javítva: a belépő assembly a BSS-ben lévő verembe mentette a bootinfo-mutatót, amit a BSS-nullázás felülírt.

### Phase 1 — Mérföldkő: saját shell (a feladatban kért első cél)

1. GDT/TSS, IDT, kivétel-dump, PIC, PIT, TSC-kalibráció, PAT, panic.
2. PMM bitmap, VMM (higher half + direkt leképezés + framebuffer WC), kheap.
3. fbcon + font + scrollback, soros tükör.
4. i8042 billentyűzet, `us`/`hu` kiosztás, line editor előzménnyel.
5. PCI-enumeráció (`disk` parancs: talált AHCI-vezérlő és NIC kiírása).
6. AOFS v1 ramdisk-olvasó, névtér, kanonizálás.
7. Kernel-módú shell: `help mem cpu disk ls cat run clear reboot bench crash`. A `run` egy AOX-fájlt kernel-szálként futtat (megbízható, Phase 2-ben ring 3-ba kerül).
8. `tests/smoke.py`, mérőszámok első rögzítése.

Kilépési feltétel:

```text
AO-OS v0.1  mem 1700M  cpu AMD C-70

AO> ls
boot.txt  hello.aox
AO> cat boot.txt
```

a netbookon, billentyűzetről, 2 másodpercen belül a BIOS-átadás után.

**Teljesítve valódi hardveren 2026-09-12.** A netbookon a shell billentyűzetről működik (`kbd hu`, `cpu`, `mem`, `bench`, `run hello`). Mért HW-értékek: VBE 0x1D4 1366×768, framebuffer 0x80000000, **pitch 5632 bájt (1408 px)**, tehát a pitch-alapú rajzolás elengedhetetlen volt; TSC 998 MHz invariáns, 2 mag, Family 20 modell 2; 19 E820-bejegyzés, 1741 MiB használható; 25 PCI-eszköz; 100 sor kiírása 2 ms write-combining módban; kernel-init 63 ms (ebből 50 ms a TSC-kalibráció). Egy HW-n talált hiba javítva: az AOX-program kimenete csak a program végén rajzolódott ki, most minden `puts`/`printf`/`getc` előtt kirajzolódik.

Korábbi állapot: QEMU-ban teljesítve. A `tests/smoke.py` 12 ellenőrzése zöld (help, mem, cpu, disk, ls, cat, run hello, echo, fb, bench, uptime, crash div → kivétel-dump). Mért értékek QEMU-ban: stage2 → prompt 96 ms, kernel.bin 49 KB, ramdisk 5,4 KB, ao.img 8 MiB. Kódméret: 3 242 sor C/asm/ld a boot+kernel+user fában (font-adat nélkül), a 8 000-es Phase 1 cél alatt. Két hiba került elő és javítva: a heap `kfree` a szabadlista-mutatóval felülírta a méretosztály-mezőt, mielőtt olvasta volna; az AOX-fájl rövidebb a `load_size`-nál, mert az objcopy a záró nullákat elhagyja.

### Phase 2 — Taskok, capability-k, írható tároló

1. Ring 3: user GDT-bejegyzések, `syscall/sysret`, kernelverem-váltás, context switch, scheduler két sorral.
2. AOX-betöltő, `user/crt0.asm`, `aolib` (syscall-wrapperek, `fmt`, string-függvények, ~500 sor).
3. Syscall-készlet (8. fejezet), pipe, `wait`/`kill`, limitek (mem, cpu, deadline).
4. `cap_check`, manifest-parser, `/sys/audit`, `caps` parancs. A `spawn` részhalmaz-ellenőrzése.
5. AHCI driver (olvasás, írás, BIOS-handoff), AOFS v2 (írás, könyvtárak, dirty-flag ellenőrzés), mount-tábla, `/state`, `/tmp`.
6. Panic-store szektor, `lastpanic`.
7. Shell átköltöztetése ring 3-ba; `ls`, `cat`, `write`, `rm`, `mkdir`, `run`, `ps`, `kill` parancsok; ACPI-kikapcsolás (`poweroff`).
8. Telepítő: `install` parancs, amely az image-et a belső SATA-lemezre írja (megerősítéssel).

Kilépési feltétel: egy `agent test.cap` manifesttel indított AOX-program írni tud a `/project/src` alá, de a `/project/Makefile` írására `E_CAP`-ot kap, és ez az audit-naplóban látszik.

### Phase 3 — Hálózat és AI-agent

1. e1000 driver (QEMU), majd RTL8101E (HW). `netdev` interfész.
2. ARP, IPv4, UDP, DHCP-kliens, minimális TCP (egy kapcsolat, egyszerű ablak, újraküldés). Cél: ~2 500 sor.
3. AOP v1 keretezés, `tools/bridge.py` (Python, a szolgáltató API-jához), `net_connect/send/recv` syscallok capability-illesztéssel.
4. `aisvc` task, `ai_open/send/recv/close`, `/state/ai/profile`.
5. `agentd`: manifest → session → tool-ciklus a 6 eszközzel, kontextus-mentés.
6. Shell: `ai "kérdés"` (egyszeri), `agent start <név> "feladat"`, `agent log`.
7. PSK + ChaCha20-Poly1305 az AOP-n.
8. Mérőszámok Phase 3 célra, kódsor-audit.

Kilépési feltétel: a netbookon a `agent start coder "írd meg a hello.c-t"` a hídon át beszél egy AI-szolgáltatóval, a `/project/src/hello.c` létrejön, a manifest-en kívüli írás elutasítva.

### Phase 4 (kitekintés, nem tervezett részletesen)

Osztott konzol (agent-napló + shell), USB-Ethernet, SMP-kísérlet, csak-olvasó FAT az adatcseréhez, lokális apró modell kísérlet.

---

## 19. Nyitott kérdések az első HW-teszt előtt

Lezárva a gépről kiolvasott adatokból (Puppy Linux, 2026-09-12):

- **Legacy BIOS-boot megerősítve:** nincs `/sys/firmware/efi`. A tervezett MBR-bootút járható, USB-ről bootol.
- **SATA = AHCI mód** (1022:7800, osztály 0106, prog-if 01). A Phase 2 AHCI-driver marad, ATA PIO tartalék nem kell.
- **NIC = Realtek RTL810xE**, 10ec:8136 rev 05, a Linux r8169 driver kezeli, RTL8208 PHY. Phase 3 driver rögzítve: r8169-regiszterkészlet.
- **Kijelző = 1366×768**, és a BIOS kínál natív VESA-módot: 0x1D4, 1366×768×32, Direct Color (grub4dos `vbeprobe`).
- **E820-térkép** ismert, 1741 MiB használható, fő blokk 1667 MiB (4.3).
- **Billentyűzet:** i8042, translate set 2→1, IRQ 1, MUX-képes vezérlő, Elantech touchpad az AUX-on.

Nincs több nyitott hardverkérdés. A Phase 0 minden bemenete megvan.
