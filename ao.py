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
    "kernel/drv/serial.c",
    "kernel/drv/fb.c",
]
KERNEL_ASM = ["kernel/arch/entry.asm"]

QEMU_MEM = "2048"


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

    print("[image]")
    cmd = [sys.executable, "tools/mkimage.py", "--stage1", b("stage1.bin"), "--stage2", b("stage2.bin"),
           "--kernel", b("kernel.bin"), "-o", b("ao.img")]
    if os.path.isfile(b("ramdisk.aofs")):
        cmd += ["--ramdisk", b("ramdisk.aofs")]
    run(cmd)

    ksize = os.path.getsize(b("kernel.bin"))
    print(f"kernel.bin: {ksize} B   stage2.bin: {os.path.getsize(b('stage2.bin'))} B   ao.img: {os.path.getsize(b('ao.img')) // 1024} KiB")


def qemu_cmd(t, extra):
    return [t["qemu"], "-machine", "pc", "-m", QEMU_MEM,
            "-drive", f"file={os.path.join(BUILD, 'ao.img')},format=raw,if=none,id=d0",
            "-device", "ahci,id=ahci", "-device", "ide-hd,drive=d0,bus=ahci.0",
            "-vga", "std", "-no-reboot", "-no-shutdown"] + extra


def cmd_run(extra=()):
    build()
    t = tools()
    run(qemu_cmd(t, ["-serial", "stdio"] + list(extra)))


def cmd_test():
    build()
    t = tools()
    log = os.path.join(BUILD, "serial.log")
    if os.path.exists(log):
        os.remove(log)
    cmd = qemu_cmd(t, ["-display", "none", "-serial", f"file:{log}"])
    print("  $", " ".join(cmd))
    p = subprocess.Popen(cmd, cwd=ROOT)
    deadline = time.time() + 15
    text = ""
    while time.time() < deadline:
        time.sleep(0.5)
        if os.path.exists(log):
            text = open(log, "r", errors="replace").read()
            if "phase0: kesz" in text or "AO>" in text:
                break
    p.kill()
    print("---- serial ----")
    print(text)
    print("----------------")
    expect = ["AO-OS v", "fb: mode=", "e820:", "phase0: kesz"]
    missing = [e for e in expect if e not in text]
    if missing:
        sys.exit(f"TESZT SIKERTELEN, hianyzik: {missing}")
    print("TESZT OK")


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
