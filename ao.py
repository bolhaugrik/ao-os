#!/usr/bin/env python3
"""AO-OS build-vezerlo.

  python ao.py tools     eszkozok ellenorzese
  python ao.py build     bootloader + kernel + lemezkep
  python ao.py run       build + QEMU (ablak + soros port a konzolon)
  python ao.py test      build + QEMU fej nelkul, soros kimenet ellenorzese
  python ao.py debug     build + QEMU gdb-stubbal (-s -S)
  python ao.py clean
  python ao.py usb \\\\.\\PhysicalDriveN   kep irasa pendrive-ra (ovatosan!)
"""
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, "build")
LOCALAPP = os.environ.get("LOCALAPPDATA", "")

TOOL_CANDIDATES = {
    "clang": ["clang", r"C:\Program Files\LLVM\bin\clang.exe"],
    "ld.lld": ["ld.lld", r"C:\Program Files\LLVM\bin\ld.lld.exe"],
    "llvm-objcopy": ["llvm-objcopy", r"C:\Program Files\LLVM\bin\llvm-objcopy.exe"],
    "nasm": ["nasm", r"C:\Program Files\NASM\nasm.exe", os.path.join(LOCALAPP, r"bin\NASM\nasm.exe")],
    "qemu": ["qemu-system-x86_64", r"C:\Program Files\qemu\qemu-system-x86_64.exe"],
}

CFLAGS = [
    "--target=x86_64-elf",
    "-ffreestanding", "-fno-builtin", "-nostdlib", "-nostdinc",
    "-fno-stack-protector", "-fno-pic", "-fno-pie",
    "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
    "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2", "-mcmodel=kernel",
    "-O2", "-g", "-std=c11",
    "-Wall", "-Wextra", "-Werror",
    "-I", os.path.join(ROOT, "kernel", "include"),
    "-I", os.path.join(ROOT, "kernel"),
]

KERNEL_C = [
    "kernel/kmain.c",
    "kernel/lib/string.c",
    "kernel/lib/fmt.c",
    "kernel/cpu/gdt.c",
    "kernel/cpu/idt.c",
    "kernel/cpu/pic.c",
    "kernel/cpu/pit.c",
    "kernel/cpu/tsc.c",
    "kernel/cpu/cpuid.c",
    "kernel/cpu/panic.c",
    "kernel/mm/pmm.c",
    "kernel/mm/vmm.c",
    "kernel/mm/kheap.c",
    "kernel/drv/serial.c",
    "kernel/drv/fb.c",
    "kernel/drv/font_data.c",
    "kernel/drv/console.c",
    "kernel/drv/kbd.c",
    "kernel/drv/pci.c",
    "kernel/drv/ahci.c",
    "kernel/drv/acpi.c",
    "kernel/drv/rtc.c",
    "kernel/drv/e1000.c",
    "kernel/drv/rtl8101.c",
    "kernel/net/net.c",
    "kernel/net/tcp.c",
    "kernel/net/dhcp.c",
    "kernel/fs/aofs.c",
    "kernel/fs/aofs2.c",
    "kernel/fs/disk.c",
    "kernel/fs/vfs.c",
    "kernel/fs/ramfs.c",
    "kernel/task/task.c",
    "kernel/task/syscall.c",
    "kernel/task/pipe.c",
    "kernel/cap/cap.c",
    "kernel/shell/shell.c",
]
KERNEL_ASM = ["kernel/arch/entry.asm", "kernel/arch/isr.asm", "kernel/task/sched.asm"]

# AOX programok: user/<nev>.c -> rootfs/bin/<nev>.aox
USER_PROGS = ["hello", "captest", "spin", "agentd", "cryptotest", "fault"]
USER_LIB = ["user/aolib.c", "user/crypto.c", "kernel/lib/fmt.c", "kernel/lib/string.c"]
USER_CFLAGS = [
    "--target=x86_64-elf",
    "-ffreestanding", "-fno-builtin", "-nostdlib", "-nostdinc",
    "-fno-stack-protector", "-fno-pic", "-fno-pie", "-fvisibility=hidden",
    "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
    "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2",
    "-O2", "-g", "-std=c11", "-Wall", "-Wextra", "-Werror",
    "-I", os.path.join(ROOT, "user"),
    "-I", os.path.join(ROOT, "kernel", "include"),
]

QEMU_MEM = "2048"
# vendeg -> host atiranyitas a QEMU user-neten (a smoke-teszt allitja): pl. "tcp:10.0.2.100:9010-tcp:127.0.0.1:9011"
QEMU_GUESTFWD = ""


def find_tool(name):
    for c in TOOL_CANDIDATES[name]:
        p = shutil.which(c) if os.sep not in c else (c if os.path.isfile(c) else None)
        if p:
            return p
    return None


def tools(strict=True):
    found = {}
    ok = True
    for name in TOOL_CANDIDATES:
        p = find_tool(name)
        found[name] = p
        print(f"  {name:13s} {p or 'HIANYZIK'}")
        if not p:
            ok = False
    if strict and not ok:
        sys.exit("hianyzo eszkoz(ok). Telepites: winget install LLVM.LLVM NASM.NASM SoftwareFreedomConservancy.QEMU")
    return found


def run(cmd, **kw):
    print("  $", " ".join(f'"{c}"' if " " in c else c for c in cmd))
    subprocess.run(cmd, check=True, cwd=ROOT, **kw)


def build():
    t = tools()
    os.makedirs(BUILD, exist_ok=True)
    b = lambda *p: os.path.join(BUILD, *p)

    print("[boot]")
    run([t["nasm"], "-f", "bin", "boot/stage1.asm", "-o", b("stage1.bin"), "-l", b("stage1.lst")])
    run([t["nasm"], "-f", "bin", "boot/stage2.asm", "-o", b("stage2.bin"), "-l", b("stage2.lst")])

    print("[kernel]")
    objs = []
    for src in KERNEL_ASM:
        obj = b(os.path.basename(src).replace(".asm", ".o"))
        run([t["nasm"], "-f", "elf64", src, "-o", obj])
        objs.append(obj)
    for src in KERNEL_C:
        obj = b(src.replace("/", "_").replace(".c", ".o"))
        run([t["clang"]] + CFLAGS + ["-c", src, "-o", obj])
        objs.append(obj)
    run([t["ld.lld"], "-T", "kernel/kernel.ld", "-nostdlib", "-static", "-z", "max-page-size=0x1000",
         "-o", b("kernel.elf")] + objs)
    run([t["llvm-objcopy"], "-O", "binary", b("kernel.elf"), b("kernel.bin")])

    print("[user]")
    os.makedirs(os.path.join(ROOT, "rootfs", "bin"), exist_ok=True)
    crt0 = b("crt0.o")
    run([t["nasm"], "-f", "elf64", "user/crt0.asm", "-o", crt0])
    libobjs = []
    for src in USER_LIB:
        obj = b("ulib_" + src.replace("/", "_").replace(".c", ".o"))
        run([t["clang"]] + USER_CFLAGS + ["-c", src, "-o", obj])
        libobjs.append(obj)
    for prog in USER_PROGS:
        obj = b(f"user_{prog}.o")
        run([t["clang"]] + USER_CFLAGS + ["-c", f"user/{prog}.c", "-o", obj])
        elf = b(f"{prog}.aox.elf")
        run([t["ld.lld"], "-T", "user/aox.ld", "-nostdlib", "-static", "--no-pie", "-z", "max-page-size=0x10",
             "-o", elf, crt0, obj] + libobjs)
        run([t["llvm-objcopy"], "-O", "binary", elf, os.path.join(ROOT, "rootfs", "bin", f"{prog}.aox")])

    print("[ramdisk]")
    # a telepitohoz a bootloader es a kernel is a ramdiskbe kerul
    os.makedirs(os.path.join(ROOT, "rootfs", "boot"), exist_ok=True)
    for f in ("stage1.bin", "stage2.bin", "kernel.bin"):
        shutil.copyfile(b(f), os.path.join(ROOT, "rootfs", "boot", f))
    run([sys.executable, "tools/mkaofs.py", "rootfs", "-o", b("ramdisk.aofs")])

    print("[image]")
    cmd = [sys.executable, "tools/mkimage.py", "--stage1", b("stage1.bin"), "--stage2", b("stage2.bin"),
           "--kernel", b("kernel.bin"), "--ramdisk", b("ramdisk.aofs"), "-o", b("ao.img")]
    run(cmd)

    ksize = os.path.getsize(b("kernel.bin"))
    print(f"kernel.bin: {ksize} B   stage2.bin: {os.path.getsize(b('stage2.bin'))} B   "
          f"ramdisk: {os.path.getsize(b('ramdisk.aofs'))} B   ao.img: {os.path.getsize(b('ao.img')) // 1024} KiB")


def qemu_cmd(t, extra):
    return [t["qemu"], "-machine", "pc", "-m", QEMU_MEM,
            "-drive", f"file={os.path.join(BUILD, 'ao.img')},format=raw,if=none,id=d0",
            "-device", "ahci,id=ahci", "-device", "ide-hd,drive=d0,bus=ahci.0",
            "-netdev", "user,id=n0" + (f",guestfwd={QEMU_GUESTFWD}" if QEMU_GUESTFWD else ""),
            "-device", "e1000,netdev=n0",
            "-vga", "std", "-no-reboot", "-no-shutdown"] + extra


def cmd_run(extra=()):
    build()
    t = tools()
    run(qemu_cmd(t, ["-serial", "stdio"] + list(extra)))


def cmd_test():
    build()
    run([sys.executable, "tests/smoke.py"])


def cmd_usb(dev):
    import ctypes
    img = os.path.join(BUILD, "ao.img")
    if not os.path.isfile(img):
        sys.exit("nincs build/ao.img, elobb: python ao.py build")
    if not dev.startswith("\\\\.\\PhysicalDrive"):
        sys.exit("adj meg \\\\.\\PhysicalDriveN alaku eszkozt")
    try:
        n = int(dev[len("\\\\.\\PhysicalDrive"):])
    except ValueError:
        sys.exit("hibas lemezszam")
    if n == 0:
        sys.exit("a 0-s lemez a rendszerlemez, erre nem irok")
    if not ctypes.windll.shell32.IsUserAnAdmin():
        sys.exit("rendszergazdai PowerShell kell (jobb klikk -> Futtatas rendszergazdakent)")

    print("Cel-lemez:")
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    f"Get-Disk -Number {n} | Format-List Number,FriendlyName,BusType,"
                    f"@{{n='SizeGB';e={{[math]::Round($_.Size/1GB,1)}}}}"], check=False)
    print(f"FIGYELEM: {dev} teljes tartalma felulirodik (particiok torolve). Folytatas: 'igen'")
    if input("> ").strip() != "igen":
        sys.exit("megszakitva")

    # A Windows nem enged nyers irast csatolt kotetek szektoraira: elobb a
    # particios tablat toroljuk diskpart-tal, igy nincs csatolt kotet.
    script = f"select disk {n}\nclean\nexit\n"
    r = subprocess.run(["diskpart"], input=script, text=True, capture_output=True)
    if r.returncode != 0:
        print(r.stdout)
        sys.exit("diskpart clean sikertelen")

    time.sleep(2)
    total = 0
    with open(img, "rb") as f, open(dev, "r+b", buffering=0) as d:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            d.write(chunk)
            total += len(chunk)
        d.flush()
    print(f"kesz: {total // 1024} KiB kiirva a {dev} eszkozre. Kihuzhatod, es indithatod rola a netbookot (F12).")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    c = sys.argv[1]
    if c == "tools":
        tools(strict=False)
    elif c == "build":
        build()
    elif c == "run":
        cmd_run(sys.argv[2:])
    elif c == "debug":
        cmd_run(["-s", "-S"])
    elif c == "test":
        cmd_test()
    elif c == "clean":
        shutil.rmtree(BUILD, ignore_errors=True)
    elif c == "usb":
        cmd_usb(sys.argv[2])
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
