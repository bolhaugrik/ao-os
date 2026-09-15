# AO-OS – a rendszer működése lépésről lépésre

Ez a leírás a bekapcsolástól a futó programokig követi végig, mi történik az AO-OS-ben, és
mindenhol a tényleges forrásra hivatkozik. Az architektúra-döntések indoklása a
[AO-OS-Architecture-v0.1.md](AO-OS-Architecture-v0.1.md)-ben, a netbook ↔ híd protokoll az
[AOP.md](AOP.md)-ben van.

## Tartalom

0. [Ami a lemezen van](#0-ami-a-lemezen-van)
1. [Bekapcsolás → BIOS → stage1](#1-bekapcsolás--bios--stage1-lba-0)
2. [stage2 – a real módú bootloader](#2-stage2--a-real-módú-bootloader-0x8000)
3. [Kernel belépés](#3-kernel-belépés--entryasm)
4. [kmain – a kernel felépül](#4-kmain--a-kernel-felépül)
5. [Az ütemező](#5-az-ütemező--hogyan-fut-egyszerre-több-dolog)
6. [A shell elindul](#6-a-shell-elindul)
7. [A capability-modell](#7-a-capability-modell--hogyan-van-korlátozva-egy-program)
8. [Megszakítások futás közben](#8-megszakítások-futás-közben)
9. [Leállás, újraindítás, frissítés](#9-leállás-újraindítás-frissítés)
10. [Összefoglaló](#10-összefoglaló)

---

## 0. Ami a lemezen van

A `python ao.py build` egy nyers lemezképet (`build/ao.img`) állít össze; ez kerül pendrive-ra,
illetve `install` után a netbook belső SSD-jére. Az elrendezés 512 bájtos szektorokban
([tools/mkimage.py](../tools/mkimage.py)):

| LBA | Tartalom |
|---|---|
| 0 | **stage1** – az MBR, 512 bájt, benne a partíciós tábla |
| 1–63 | **stage2** – real módú bootloader (max 63 szektor ≈ 31 KiB) |
| 64– | **kernel.bin** – a 64 bites kernel, lapos bináris |
| a kernel után | **ramdisk** – AOFS v1 kép (a `rootfs/` mappa: `/bin/*.aox`, `/etc/agents/*.cap`, …) |
| 2048– | **AOFS v2 partíció** – az írható lemez-fájlrendszer (`/state`, `/project`) |

A stage2 elején egy fejléc van (`'AOS2'` + a kernel és a ramdisk LBA-ja és szektorszáma), ezt a
mkimage tölti ki. A bootloader így nem ismer fájlrendszert: csak „olvasd be az X. szektortól Y
darabot” utasításokat ad a BIOS-nak.

---

## 1. Bekapcsolás → BIOS → stage1 (LBA 0)

Forrás: [boot/stage1.asm](../boot/stage1.asm)

1. A BIOS (a netbook legacy BIOS-a vagy a QEMU SeaBIOS-a) a POST után beolvassa a boot-eszköz 0.
   szektorát a `0x7C00` címre, és ha a végén `0xAA55` áll, odaugrik. A `DL` regiszterben a
   boot-meghajtó száma van.
2. A stage1 16 bites real módban indul:
   - normalizálja a `CS:IP`-t (`jmp 0x0000:.norm`), nullázza a szegmenseket, a vermet `0x7C00`
     alá teszi, elmenti a `DL`-t;
   - kiírja az **`1`** karaktert BIOS teletype hívással (INT 10h / AH=0Eh) – ez az első boot-jelző;
   - megkérdezi a BIOS-t (INT 13h / AH=41h), tud-e LBA-s bővített olvasást; ha nem → **`E`**, megáll;
   - 16 szektoros darabokban (USB-BIOS-barát méret) beolvassa az LBA 1..63-at a `0x8000` címre
     egy DAP-struktúrával (INT 13h / AH=42h); lemezhiba → **`D`**;
   - `jmp 0x0000:0x8000` – átadja a vezérlést a stage2-nek.

> **Példa.** Ha bekapcsolás után csak `1D` látszik a képernyőn, a stage1 elindult, de a stage2
> szektorait nem tudta beolvasni (rossz pendrive, hibás írás).

---

## 2. stage2 – a real módú bootloader (0x8000)

Forrás: [boot/stage2.asm](../boot/stage2.asm)

Minden lépés után egy betűt ír a képernyőre és a soros portra:

| Jelző | Mit csinál | Hiba |
|---|---|---|
| **2** | Verem `0x7B00`-ra; COM1 inicializálása (115200 8N1 – a QEMU-hoz, a netbookon nincs port, ártalmatlan). Elkezdi kitölteni a **bootinfo** struktúrát `0x1000`-en: magic `'AOBI'`, `rdtsc` (a boot-idő méréséhez), boot-drive. | |
| **A** | **A20-kapu**: először teszteli (a `0x0500` és a `0x100500` ugyanaz a bájt?), aztán INT 15h/2401h, ha az sem, a `0x92`-es port („Fast A20”). Nélküle az 1 MiB fölötti címek „átfordulnának”. | `a` |
| **U** | **Unreal mód**: átvált védett módba egy 4 GiB-es adatszegmenssel, betölti DS/ES-be, visszavált real módba. A szegmens-cache megőrzi a 4 GiB-es limitet, így 16 bites kódból is írhat 1 MiB fölé (`a32 rep movsd`). | |
| **M** | **E820 memóriatérkép**: INT 15h/E820h ciklusban, max 64 bejegyzés `0x1200`-ra. A kernel ebből tudja meg, melyik RAM-tartomány használható. | `m` |
| **K** | **Kernel beolvasása** az 1 MiB-es fizikai címre (`0x100000`). A BIOS csak az első 1 MiB-be tud írni, ezért egy 16 KiB-os *bounce-puffert* használ `0x10000`-en: INT 13h oda olvas 32 szektort, majd unreal módban átmásolja a célra. Lemezhibánál 3× újrapróbál (reset + olvasás). | `d` |
| **R** | **Ramdisk beolvasása** a 4 MiB-es címre (`0x400000`), ugyanígy. Kimarad, ha a fejléc szerint nincs ramdisk. | `d` |
| **V** | **VBE grafikus mód**: lekéri a módlistát (INT 10h/4F00h), és a jelöltek sorrendjében (`1366×768 → 1360×768 → 1280×720 → 1024×768 → 800×600`) keres 32 bpp, direct color, lineáris framebufferes módot. A bootinfóba írja a framebuffer fizikai címét, méretét, pitch-ét, RGB-pozícióit, majd beállítja (`4F02h`, bit 14 = LFB). **Innentől nincs szöveges mód: a `V` és a `P` már csak a soros portra megy.** | `v` |
| **P** | **Lapozótáblák** `0x70000`-től (32 KiB): PML4 → PDPT → 4 PD, 2048 db 2 MiB-es lap. Három leképezés mutat ugyanarra a fizikai 0–4 GiB-re: identity (virt 0 = fiz 0), *direct map* (`0xFFFF8000_00000000` + fiz) és a *kernel higher-half* (`0xFFFFFFFF_80000000` = fiz 0). | |

Ezután `enter_long`: CR4.PAE = 1, CR3 = `0x70000`, EFER.LME = 1 (MSR `0xC0000080`), 64 bites GDT,
CR0.PG|PE → `jmp 0x08:long_entry`. 64 bites módban beállítja a szegmenseket, `RDI = 0x1000`
(a bootinfo fizikai címe), és `jmp 0x100000` – a kernelre.

> **Példa.** A netbookon a képernyőn `12AUMKR` villan fel, majd elsötétül, és megjelenik a grafikus
> konzol. Ha `12AUMKRv` látszik és megáll, a VBE egyik jelölt-felbontást sem adta 32 bpp-ben.

A bootinfo mezői ([kernel/include/bootinfo.h](../kernel/include/bootinfo.h)):
magic, méret, TSC, E820 darab + cím, framebuffer (cím, szélesség, magasság, pitch, bpp, RGB
pozíciók, VBE mód), kernel cím + méret, ramdisk cím + méret, boot-drive.

---

## 3. Kernel belépés – entry.asm

Forrás: [kernel/arch/entry.asm](../kernel/arch/entry.asm), [kernel/kernel.ld](../kernel/kernel.ld)

A kernel a `0xFFFFFFFF80100000` virtuális címre van linkelve, de a stage2 a fizikai `0x100000`-re
ugrott (identity map). Az `_start`:

1. `mov rax, .high; jmp rax` – átugrik a higher-half virtuális címre (ugyanaz a fizikai kód, csak
   most már a kernel saját címterében fut).
2. Beállítja a kernel 16 KiB-os boot-vermét (`kstack` a bss-ben).
3. **Nullázza a bss-t** – mivel a verem is a bss-ben van, az `RDI`-t (bootinfo) előbb `RBX`-be menti.
4. A bootinfo fizikai címét direct-map virtuálisra alakítja (`+ 0xFFFF8000_00000000`).
5. `call kmain`.

---

## 4. kmain – a kernel felépül

Forrás: [kernel/kmain.c](../kernel/kmain.c)

Sorrendben, `mark()` időbélyegekkel (ezek a boot végén `+N us` formában kiíródnak):

1. **Soros port**, bootinfo magic ellenőrzés (`'AOBI'`, különben halt).
2. **CPU** – `gdt_init` (kernel/user kód+adat szegmensek, TSS), `idt_init` (256 vektor; kivétel →
   panic dump, IRQ → regisztrált kezelő), `pic_init` (8259 átprogramozva, IRQ0–15 → 32–47),
   `pit_init` (100 Hz tick), `sti()`.
3. **Memória + konzol**
   - `pmm_init` – az E820-ból bitmap a szabad 4 KiB-os frame-ekről ([mm/pmm.c](../kernel/mm/pmm.c)).
   - `vmm_init` – átveszi a stage2 lapozótábláit; ez lesz a „boot PML4” ([mm/vmm.c](../kernel/mm/vmm.c)).
   - `fb_init` – a framebuffer a bootinfo alapján; `vmm_set_wc` write-combining PAT-tal (nélküle
     a pixelírás lassú).
   - `kheap_init` – kernel kupac (`kmalloc` / `kfree`).
   - `console_init` – szöveges konzol a framebufferre, saját bitmapfonttal, UTF-8-cal, görgetéssel.
     **Innentől működik a `kprintf`**; az első sor: `AO-OS v… boot: OK`.
4. **Driverek** – `tsc_calibrate` (TSC frekvencia a PIT-hez mérve), `kbd_init` (i8042, IRQ1, US/HU
   kiosztás), `pci_init` (buszbejárás).
5. **Névtér** ([fs/vfs.c](../kernel/fs/vfs.c))
   - `vfs_init` → mount-tábla.
   - A ramdisk (AOFS v1, csak olvasható) a `/`-re: `aofs_mount(P2V(ramdisk_paddr))` – nem másol,
     közvetlenül a 4 MiB-es címen lévő képet használja.
   - `/tmp` és `/sys` → ramfs. A `/sys/audit` és `/sys/version` *generált* fájlok: olvasáskor egy
     C függvény állítja elő a tartalmukat.
6. **Lemez** – `ahci_init` (PCI-n talált AHCI vezérlő, port, ATA IDENTIFY), majd
   `disk_mount_root` ([fs/disk.c](../kernel/fs/disk.c)): megkeresi a `0x7F` típusú partíciót,
   `aofs2_mount` (4 KiB blokkok, piszkos-jelző: ha az előző leállás nem volt tiszta, újraszámolja a
   bitmapet). **Ekkor a lemez lesz a `/`, a ramdisk pedig átköltözik `/rd`-re.** Ha nincs partíció,
   marad a ramdisk gyökér, és a shell `install`-t kínál.
7. `acpi_init` (az `_S5` a poweroffhoz), `net_init` + `e1000_init || rtl8101_init` (QEMU, illetve a
   netbook Realtek kártyája).
8. **Taskok** – `task_init`: FPU/SSE engedélyezése, idle task (pid 0), a `syscall` / `sysret`
   MSR-ek (STAR, LSTAR → `syscall_entry`, SFMASK). Majd:
   - `task_create_kernel("shell", shell_thread)` – kernel-szál;
   - `task_create_kernel("net", net_thread)` – ha van hálózati kártya: 200 ms várakozás, **DHCP**
     (max 8 s), majd AOP PING a hídnak, hogy a splash mutathassa, elérhető-e;
   - `screenshot_init` – F12.
9. `task_idle_loop()` – a kmain innen sosem tér vissza: `cli; schedule(); hlt`.

> **Példa a képernyőn:**
> ```
> AO-OS v0.7 boot: OK
> fb: 0xd0000000 1366x768 pitch 5464 (WC)  e820: 9 bejegyzes  frame: 498112 szabad / 524288
> ramdisk: AOFS v1, 38 bejegyzes, 1620 KiB, csatolva: /
> ahci: port 0, KINGSTON SSD, 30528 MiB
> lemez: AOFS v2 'ao' LBA 2048, 65536 blokk (61200 szabad), 214 inode, csatolva: /
> acpi: OK (poweroff elerheto)
> net: rtl8101  mac 88:ae:1d:...
> pci: 14 eszkoz   tsc: 1000 MHz   kbd: i8042
>   cpu      +112 us
>   mem+con  +8400 us
>   drv      +51000 us
> ```

---

## 5. Az ütemező – hogyan fut egyszerre több dolog

Forrás: [kernel/task/task.c](../kernel/task/task.c), [kernel/task/sched.asm](../kernel/task/sched.asm)

- Legfeljebb **32 task**; mindegyiknek saját kernel-verme (32 KiB), saját PML4-e (user-taskoknál),
  16 handle-je, capability-készlete, korlátai, FPU-állapota (512 bájtos `fxsave` terület).
- **Round-robin.** A PIT 100 Hz-cel hív `task_tick`-et: ébreszti az alvókat és az időzített
  várakozókat, számolja a user-task CPU-idejét, ellenőrzi a `cpu` és `deadline` korlátot
  (túllépés → `killed = true`), és 2 tick (20 ms) kvantum után `need_resched`-et jelez.
- **A kernel nem preemptálható.** Megszakításból csak akkor váltunk taskot, ha a megszakítás ring
  3-ból vagy az idle-ből jött (`task_preempt_check`). A kernel-szálak (shell, net) csak *önként*
  adják át a vezérlést: `task_block_on(waitq)` (pl. billentyűre vár), `task_sleep_ms`,
  `task_wait`. Ettől egyszerű a kernel: nincs zárolás-káosz.
- **Kontextusváltás** (`switch_to`): elmenti a callee-saved regisztereket és az FPU-állapotot,
  kicseréli az `RSP`-t, visszatölti az újat. Ha más a PML4, `vmm_switch` (CR3 írás). A TSS-ben
  frissül a kernel-verem teteje, hogy a következő ring 3 → 0 átmenet a jó veremre essen.
- **Új task első futása.** A `build_frame` egy kész megszakítás-keretet épít a kernel-vermen
  (`ss, rsp, rflags = IF, cs, rip, …`); a `switch_to` „visszatér” a `task_trampoline`-ba, ami
  `jmp isr_return` → `iretq` → a task a megadott `rip`-en, a megadott privilégiumszinten indul.
  Ugyanez a mechanizmus szolgál kernel-szálnak (`cs = kernel`) és user-tasknak (`cs = user|3`).

Task-állapotok: `T_FREE → T_READY ⇄ T_RUNNING → T_BLOCKED → … → T_ZOMBIE → T_FREE`
(a zombit a szülő `task_wait`-je vagy a `ps` takarítja el).

---

## 6. A shell elindul

Forrás: [kernel/shell/shell.c](../kernel/shell/shell.c), `shell_run`

1. Kiszámolja a boot-időt (`rdtsc − bootinfo.tsc`) – ez a „~210 ms boot → prompt”.
2. `disk_sync_from_ramdisk`: összeveti a `/rd/etc/build` és az `/etc/build` bélyeget; ha a ramdisk
   újabb (hálózati `update` után), átmásolja a `/rd/bin` és `/rd/etc` tartalmát a lemezre. (Azért a
   shell-szálban és nem a kmainben, mert a rekurzív másoláshoz a 32 KiB-os task-verem kell; a
   16 KiB-os boot-verem kevés.)
3. `splash()` – fejléc build-bélyeggel, `motd`.
4. `run_rc()` – a `/state/rc` (ha nincs, az `/etc/rc`) sorait futtatja parancsként.
   Példa: `append /state/rc kbd hu` → minden bootnál magyar kiosztás.
5. Végtelen ciklus: prompt (`AO> ` vagy `AO /project> `), `read_line_init` (sorszerkesztés,
   history, Tab-kiegészítés; közben a shell **blokkol** a billentyűzet várakozási során, tehát nem
   fogyaszt CPU-t), majd `execute(line)`.

### 6.1 Példa: beépített parancs – `cat /etc/motd`

`execute` → `split` → `argv = {"cat", "/etc/motd"}` → az `if/else` lánc `cmd_cat`-ra irányít →
`vfs_read_all("/etc/motd")` → a VFS a mount-táblában a leghosszabb illeszkedő prefixet keresi
(`/` → aofs2) → az AOFS v2 inode-ból blokkok az AHCI-n át → `kprintf`. Minden a kernelben,
ring 0-ban, egyetlen szálon; nem indul új task.

### 6.2 Példa: user-program – `run hello`

1. `cmd_run` → `run_with_caps(&shell->caps, …)`: a shell a **root** capability-készletét adja
   (`fs.read **`, `fs.write **`, `exec **`, `net *`, `spawn`, `sysinfo`, `power`, `console`, `fb`).
2. `find_prog("hello")`: sorban `hello` → `/bin/hello` → `/bin/hello.aox` → `/rd/bin/hello.aox` →
   `/state/bin/hello.aox`.
3. `vfs_read_all` beolvassa a fájlt a kupacra, majd `task_create_user`:
   - ellenőrzi az **AOX fejlécet** (`'AOX1'`, entry < load_size, load_size ≤ 16 MiB). Az AOX
     *lapos* kép: 32 bájt fejléc + kód + adat, relokáció nélkül, fixen `0x400000`-re linkelve
     ([user/aox.ld](../user/aox.ld), [kernel/include/aox.h](../kernel/include/aox.h));
   - `vmm_new_space()` – új PML4: a kernel felső fele megosztott, az alsó fele üres;
   - lapokat foglal `0x400000`-től a képnek + bss-nek (`map_user_pages`, ami a `mem` korlátot is
     számolja), és lapról lapra bemásolja a képet;
   - vermet foglal a `0x7FFF_0000_0000` alá (a fejléc `stack_size`-a, 16 KiB … 1 MiB), az `argv`
     stringeket és mutatótömböt a legfelső lapra írja;
   - handle-ök: 0 = konzol be, 1–2 = konzol ki;
   - `build_frame(rip = 0x400000 + entry, cs = user|3, rsp = user_rsp, rdi = argc, rsi = argv)`
     → `T_READY`.
4. A shell `task_wait(pid)`-del **blokkol** a gyerek `exit_q`-ján. Az ütemező a `hello` taskra
   vált → `iretq` → **ring 3**, a [user/crt0.asm](../user/crt0.asm) `_start`-ja:
   `and rsp, -16; call main`.
5. A `hello` `main`-je `ao_printf`-et hív → [user/aolib.c](../user/aolib.c) `ao_write(1, buf, n)`
   → `syscall` utasítás, `rax = SYS_WRITE`, argumentumok `rdi rsi rdx r10 r8`-ban.
6. **`syscall_entry`**: a CPU a LSTAR címre ugrik ring 0-ban, IF = 0. Elmenti a user `rsp`-t,
   átvált a task kernel-vermére, *ugyanolyan `struct regs` keretet épít, mint a megszakítások*
   (ss/rsp/rflags/cs/rip + 15 regiszter), `sti`, `call syscall_dispatch`.
7. `syscall_dispatch` ([kernel/task/syscall.c](../kernel/task/syscall.c)) → `sys_write`:
   `user_range_ok` (a puffer a task kép- vagy verem-tartományában van-e – ez védi a kernelt a
   hamis mutatóktól), `cap_check(t, CAP_CONSOLE)`, majd `console_putc` karakterenként,
   `console_flush`.
8. Visszaút: regiszterek vissza, `rcx = rip`, `r11 = rflags`, `pop rsp`, **`sysret`** → ring 3,
   a `main` folytatódik.
9. `main` visszatér → `ao_exit(rc)` → `SYS_EXIT` → `task_exit`: handle-ök zárása, konzolmód és
   framebuffer visszaállítása, `vmm_destroy_space` (minden user-lap visszakerül a PMM-be),
   `T_ZOMBIE`, `waitq_wake_all(exit_q)` → a shell felébred, `free_task`, kiírja:
   `[hello: pid 7, rc=0, 3 ms]`.

Ha a program hibázik (pl. null-mutató), a page fault ring 3-ból jön → az IDT kezelője nem
panicol, hanem `task_exit(E_FAULT)` → a shell `rc=-17 (kivetel)`-t lát, a rendszer megy tovább.
A `run fault` pont ezt demonstrálja.

### 6.3 Címtér-elrendezés egy user-tasknál

```
0x0000_0000_0040_0000  AOX kép (kód, rodata, adat) + bss            ← USER_LOAD
        …              heap (SYS_SBRK-val nő, a mem-korlátig)
0x0000_7FFF_0000_0000  verem teteje (argv a legfelső lapon)          ← USER_STACK_TOP
0xFFFF_8000_0000_0000  direct map (fizikai memória tükre)            ← csak ring 0
0xFFFF_FFFF_8000_0000  kernel (higher-half)                          ← csak ring 0
```

---

## 7. A capability-modell – hogyan van korlátozva egy program

Forrás: [kernel/cap/cap.c](../kernel/cap/cap.c), [kernel/cap/cap.h](../kernel/cap/cap.h)

Nincs felhasználó, nincs rwx bit. Egy task egy **capability-készletet** birtokol (max 16 db
`fajta + glob-minta`, plusz `mem` / `cpu` / `deadline` korlát), és minden erőforrást megnevező
syscall `cap_check`-en megy át:

| Syscall | Ellenőrzés |
|---|---|
| `open(path, O_READ)` | `fs.read` illik-e a *kanonizált* útvonalra |
| `open(path, O_WRITE\|O_CREATE…)`, `mkdir`, `unlink` | `fs.write` |
| `spawn(path, …)` | `spawn` + `exec` a program útvonalára; a gyerek manifestje ⊆ a szülő készlete |
| `net_connect("ip:port")` | `net` minta (`*:9010` = bármely host, csak a híd portja) |
| `read` / `write` a konzolra, `con_mode` | `console` |
| `fb_map` | `fb` |
| `sysinfo` | `sysinfo` |
| `reboot` / `poweroff` | `power` |

A glob: `*` egy útvonal-komponensen belül, `**` tetszőleges mélységben. Elutasításnál bejegyzés
kerül a 64 elemű **audit-gyűrűbe**, ami `cat /sys/audit`-tal olvasható:

```
1234 pid=9 fs.write deny /etc/ai/bridge
```

A készlet **csak szűkíthető**: `capset_subset(child, parent)` minden gyerek-capre megköveteli,
hogy legyen egy szülő-cap ugyanazzal a fajtával, amelynek mintája lefedi; a korlátok sem lehetnek
nagyobbak. Így egy `coder` agent nem tud nagyobb jogú gyereket indítani, mint amennyi neki van.

### 7.1 Példa: `agent coder "írj egy hello.c-t a /project/src alá"`

1. `cmd_agent("coder", "agentd", …)` megkeresi a manifestet: `/state/agents/coder.cap`, ha
   nincs, `/etc/agents/coder.cap` ([rootfs/etc/agents/coder.cap](../rootfs/etc/agents/coder.cap)):
   ```
   agent coder
   console
   net       *:9010
   fs.read   /etc/ai/**
   fs.read   /state/ai/**
   fs.read   /state/agents/coder/**
   fs.write  /state/agents/coder/**
   fs.read   /project/**
   fs.write  /project/src/**
   exec      /bin/**
   spawn
   mem       16M
   cpu       120s
   deadline  1800s
   ```
2. `capset_parse` → `run_with_caps(&cs, {"agentd", "írj egy…"})`. Az `agentd.aox` tehát **ezekkel**
   a jogokkal indul user-taskként.
3. Az `agentd` ([user/agentd.c](../user/agentd.c)) `SYS_NET_CONNECT`-tel a hídhoz kapcsolódik
   (`/state/ai/bridge` = `ip:9010`; a `cap_check(CAP_NET, "192.168.1.10:9010")` illik a `*:9010`-re).
   AOP kereteket küld: `HELLO agent=coder`, `CONTEXT` (a saját capability-listája – így a modell
   tudja, mit tehet), `PROMPT`. A csatorna ChaCha20-Poly1305-tel titkosított a `/state/ai/psk`
   kulccsal; az API-kulcs a PC-n marad.
4. A híd ([tools/bridge.py](../tools/bridge.py)) hívja a Claude / Gemini API-t, és a modell
   eszközhívásait `TOOL_CALL` keretként továbbítja. Mind a hat eszköz egy-egy syscall:
   `fs_write path=/project/src/hello.c` → `ao_open(path, O_WRITE|O_CREATE|O_TRUNC)` → a kernelben
   `cap_check(CAP_FS_WRITE, "/project/src/hello.c")` → illik a `/project/src/**`-ra → engedélyezve.
5. Ha a modell az `/etc/ai/bridge`-be próbálna írni: `cap_check` → **`E_CAP`**, audit-bejegyzés,
   az `agentd` `TOOL_RESULT error`-t küld vissza, a modell más utat próbál.
   **A kernel kényszeríti ki a manifestet, nem az agentd jóindulata.**
6. `task_run` → `ao_spawn("/bin/…")` → `SYS_SPAWN`: `spawn` + `exec /bin/**` kell hozzá; a gyerek
   örökli (vagy tovább szűkíti) a készletet.
7. Ha a 120 s CPU vagy a 30 perces határidő letelik: `task_tick` → `killed`, a következő
   syscall / megszakítás visszaútján `task_exit(E_TIMEOUT)` → a shell `rc=-9 (hatarido/kill)`.

### 7.2 Példa: `doom`

`cmd_agent("doom", "/state/games/doom.aox")` a `doom.cap`-pal: `console`, `sysinfo`, `fb`,
`fs.read/write /state/games/**`, `mem 96M` – **hálózat nincs**.

- A Doom `SYS_FB_MAP`-ot hív → `cap_check(CAP_FB)` → a kernel a framebuffer fizikai lapjait a task
  címterébe képezi és `console_suspend(true)`; a program közvetlenül írja a pixeleket.
- `SYS_CON_MODE(CON_RAW | CON_NONBLOCK)` → nyers `key_ev` rekordok (lenyomás és felengedés,
  módosítók), blokkolás nélkül – ez a játékciklus.
- A PureDOOM `malloc`-ja `SYS_SBRK`-val kér lapokat, a 96 MiB-es korlátig.
- Az FPU/SSE-állapotot a kernel minden taskváltásnál menti (`fxsave` / `fxrstor`).
- Kilépéskor `task_exit` visszaállítja a konzolt és a szöveges billentyűmódot.

---

## 8. Megszakítások futás közben

Forrás: [kernel/cpu/idt.c](../kernel/cpu/idt.c), [kernel/arch/isr.asm](../kernel/arch/isr.asm)

- **IRQ0 (PIT, 100 Hz)** → `pit_irq` → `task_tick` → EOI → `task_preempt_check`: ha ring 3-ból
  jött és lejárt a kvantum, `schedule()`. Ha a kernelben jött (pl. a shell éppen AOFS-t ír), *nem*
  váltunk – a kernel atomi marad.
- **IRQ1 (billentyűzet)** → `kbd_irq`: scancode → kiosztás (US/HU, módosítók) → `key_event` a
  gyűrűbe → `waitq_wake_all` a várakozó tasknak (shell vagy user-program `read(0)`-ja). F12-t itt
  kapja el a `screenshot`, Ctrl+C-re `task_kill_user_all`.
- **Hálózati IRQ** → e1000 / rtl8101 → Ethernet → ARP / IPv4 → ICMP / UDP (DHCP) / TCP → a TCP a
  socket handle várakozási sorát ébreszti ([kernel/net/](../kernel/net/)).
- **Kivétel ring 3-ból** → a task `task_exit(E_FAULT)`-tal áll le, a rendszer megy tovább.
- **Kivétel ring 0-ból** → panic dump (regiszterek, verem) a konzolra és a soros portra, plusz a
  *panic-tároló szektorba* a lemezen, hogy újraindítás után is olvasható legyen.

---

## 9. Leállás, újraindítás, frissítés

- `poweroff` → `cap_check(CAP_SYS_POWER)` → az AOFS v2 törli a piszkos-jelzőt → ACPI `_S5`.
- `update` → a hídon át lehúzza a `share/boot.img`-et (1 MiB: stage1 + stage2 + kernel + ramdisk),
  ellenőrzi, a lemez 0–2047. szektorára írja, újraindít. A következő bootnál a 6. fejezet 2. pontja
  (`disk_sync_from_ramdisk`) frissíti a `/bin`-t és az `/etc`-t; a `/state` és a `/project`
  érintetlen marad.
- `install` (pendrive-ról bootolva) → a teljes lemezkép a belső SSD-re, a `/state`, `/state/agents`,
  `/project` könyvtárak létrehozása.

---

## 10. Összefoglaló

```
BIOS
 └─ stage1 (MBR, 512 B) – stage2 beolvasása
     └─ stage2 (real mód) – A20, unreal, E820, kernel + ramdisk → RAM, VBE, lapozótáblák, long mode
         └─ entry.asm – higher-half ugrás, bss nullázás
             └─ kmain – GDT/IDT/PIC/PIT, PMM/VMM/heap, konzol, driverek, VFS (ramdisk, /tmp, /sys),
                │        AHCI + AOFS v2 (/), ACPI, hálózat, task_init
                ├─ shell (kernel-szál) – sync a ramdiskből, splash, rc, prompt-ciklus
                │    ├─ beépített parancs: a kernelben fut (cat, ls, edit, …)
                │    └─ AOX program: saját címtér, ring 3, capability-készlet,
                │         minden syscall → cap_check
                ├─ net (kernel-szál) – DHCP, híd PING
                └─ idle – hlt
```

Egy mondatban: a shell soronként vagy egy beépített parancsot futtat, vagy egy AOX képet tölt saját
címtérbe ring 3-ba egy capability-készlettel, és minden syscall a `cap_check`-en megy át – így egy
távoli AI-modell is csak azt teheti a gépen, amit a manifestje enged.
