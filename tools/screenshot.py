#!/usr/bin/env python3
"""QEMU inditasa fej nelkul, kepernyokep keszitese a monitoron at (screendump), PNG-be mentve.
Hasznalat: python tools/screenshot.py [kimenet.png]  (a build/ao.img-t hasznalja)"""
import os
import struct
import subprocess
import sys
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import ao  # noqa: E402


def ppm_to_png(ppm_path, png_path):
    data = open(ppm_path, "rb").read()
    # P6\n<w> <h>\n255\n<rgb...>
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    raw = parts[3]
    rows = b"".join(b"\0" + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, b):
        c = t + b
        return struct.pack(">I", len(b)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
    open(png_path, "wb").write(png)
    return w, h


def main():
    positional = [a for i, a in enumerate(sys.argv) if i > 0 and not a.startswith("--") and sys.argv[i - 1] != "--cmd"]
    out = positional[0] if positional else os.path.join(ao.BUILD, "screen.png")
    t = ao.tools()
    ppm = os.path.join(ao.BUILD, "screen.ppm")
    if os.path.exists(ppm):
        os.remove(ppm)
    import socket
    port = 4488
    # opcionalis parancsok a soros porton at: --cmd "..." (tobbszor is)
    cmds = [a for i, a in enumerate(sys.argv) if i > 0 and sys.argv[i - 1] == "--cmd"]
    serial_port = 4489
    serial_arg = f"tcp:127.0.0.1:{serial_port},server,nowait" if cmds else "null"
    cmd = ao.qemu_cmd(t, ["-display", "none", "-serial", serial_arg,
                          "-monitor", f"tcp:127.0.0.1:{port},server,nowait"])
    p = subprocess.Popen(cmd, cwd=ROOT)
    time.sleep(4)
    if cmds:
        ser = socket.create_connection(("127.0.0.1", serial_port), timeout=5)
        for c in cmds:
            ser.sendall(c.encode("utf-8") + b"\n")
            time.sleep(1.5)
        ser.close()
        time.sleep(1)
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.settimeout(1)
    try:
        s.recv(4096)
    except socket.timeout:
        pass
    s.sendall(f"screendump {ppm.replace(os.sep, '/')}\n".encode())
    time.sleep(2)
    try:
        print(s.recv(4096).decode(errors="replace"))
    except socket.timeout:
        pass
    s.sendall(b"quit\n")
    s.close()
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()
    w, h = ppm_to_png(ppm, out)
    print(f"{out}  {w}x{h}")


if __name__ == "__main__":
    main()
