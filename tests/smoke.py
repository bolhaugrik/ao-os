#!/usr/bin/env python3
"""Fej nelkuli QEMU smoke-teszt: parancsok a soros porton at, kimenet ellenorzese.
Hasznalat: python tests/smoke.py  (a build/ao.img-t hasznalja)"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import ao  # noqa: E402

PORT = 4499
CMDS = [
    ("help", "AO-OS parancsok"),
    ("mem", "hasznalhato:"),
    ("cpu", "tsc:"),
    ("disk", "ramdisk (AOFS v1): csatolva"),
    ("ls", "boot.txt"),
    ("cat boot.txt", "AO-OS ramdisk"),
    ("run hello x y\n", "hello az AOX-bol"),
    ("echo proba 123", "proba 123"),
    ("fb", "konzol"),
    ("bench", "boot -> prompt"),
    ("uptime", " s"),
]


def recv_until(s, needle, timeout):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            break
        buf += chunk
        if needle.encode("latin-1") in buf:
            return buf.decode("latin-1", errors="replace"), True
    return buf.decode("latin-1", errors="replace"), False


def main():
    t = ao.tools()
    # 'server' nowait nelkul: a QEMU megvarja a kliens csatlakozasat, igy a boot-kimenet nem vesz el
    cmd = ao.qemu_cmd(t, ["-display", "none", "-serial", f"tcp:127.0.0.1:{PORT},server"])
    p = subprocess.Popen(cmd, cwd=ROOT)
    s = None
    for _ in range(40):
        time.sleep(0.25)
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
            break
        except OSError:
            continue
    if s is None:
        p.kill()
        sys.exit("nem sikerult a QEMU soros portjara csatlakozni")
    s.settimeout(0.5)
    ok = True
    log = []
    out, found = recv_until(s, "AO> ", 15)
    log.append(out)
    if not found:
        print(out)
        p.kill()
        sys.exit("nem jott prompt")
    t_boot = out
    for c, expect in CMDS:
        s.sendall(c.encode("latin-1") + b"\n")
        out, found = recv_until(s, expect, 8)
        log.append(out)
        # a prompt visszaerkezese
        rest, _ = recv_until(s, "AO> ", 3)
        log.append(rest)
        print(f"  {'OK ' if found else 'FAIL'}  {c.strip():24s} -> {expect!r}")
        ok &= found
    s.sendall(b"crash div\n")
    out, found = recv_until(s, "KIVETEL 0: divide error", 5)
    log.append(out)
    print(f"  {'OK ' if found else 'FAIL'}  {'crash div':24s} -> kivetel-dump")
    ok &= found
    p.kill()
    full = "".join(log)
    with open(os.path.join(ao.BUILD, "smoke.log"), "w", encoding="latin-1", errors="replace") as f:
        f.write(full)
    if "boot -> prompt" in full:
        for line in full.splitlines():
            if "boot -> prompt" in line or "utolso billentyu" in line or "100 sor" in line:
                print("  " + line.strip())
    print("SMOKE OK" if ok else "SMOKE FAIL (build/smoke.log)")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
