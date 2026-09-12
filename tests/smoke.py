#!/usr/bin/env python3
"""Fej nelkuli QEMU smoke-teszt: parancsok a soros porton at, tobb boot-menetben.
Hasznalat: python tests/smoke.py  (a build/ao.img-t hasznalja; a lemezkep modosul!)"""
import os
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import ao  # noqa: E402

PORT = 4499

# (parancs, elvart reszlet a kimenetben); a "\n" a parancs vegen extra Entert kuld
SESSIONS = [
    [   # 1. menet: alap parancsok, taskok, capability, formazas, perzisztens iras, panic
        ("help", "AO-OS parancsok"),
        ("mem", "hasznalhato:"),
        ("cpu", "tsc:"),
        ("disk", "AO-particio"),
        ("ls", "boot.txt"),
        ("cat boot.txt", "AO-OS ramdisk"),
        ("mount", "ramfs"),
        ("run hello x y\n", "rc=0"),
        ("write /tmp/t.txt proba szoveg", "bajt"),
        ("cat /tmp/t.txt", "proba szoveg"),
        ("mkdir /tmp/project", "AO> "),
        ("mkdir /tmp/project/src", "AO> "),
        ("spawn /etc/agent-test.cap captest", "rc=0"),
        ("cat /tmp/project/src/a.txt", "agent irta"),
        ("cat /tmp/project/Makefile", "nincs ilyen"),
        ("audit", "ELUTASITVA  /tmp/project/Makefile"),
        ("spawn /etc/agent-spin.cap spin", "hatarido/kill"),
        ("ps", "shell"),
        ("echo proba 123", "proba 123"),
        ("fb", "konzol"),
        ("bench", "boot -> prompt"),
        ("uptime", " s"),
        ("net", "ip 10.0.2.15"),
        ("ping 10.0.2.2", "valasz, rtt"),
        ("nc 10.0.2.2 4590 hello ao", "echo: hello ao"),
        ("mkfs", "IGEN"),
        ("IGEN", "csatolva: /"),
        ("mount", "aofs2"),
        ("mkdir /state", "AO> "),
        ("write /state/x.txt perzisztens proba", "bajt"),
        ("mkdir /state/d", "AO> "),
        ("write /state/d/y.txt masodik", "bajt"),
        ("ls /state", "d"),
        ("sync", "sync: ok"),
        ("crash div", "KIVETEL 0: divide error"),
    ],
    [   # 2. menet: a lemez a gyoker, perzisztencia, panic-tarolo, telepites
        ("mount", "aofs2"),
        ("cat /state/x.txt", "perzisztens proba"),
        ("cat /state/d/y.txt", "masodik"),
        ("lastpanic", "KIVETEL 0 divide error"),
        ("lastpanic clear", "torolve"),
        ("ls /rd/bin", "hello.aox"),
        ("run hello\n", "rc=0"),
        ("rm /state/d/y.txt", "AO> "),
        ("rm /state/d", "AO> "),
        ("ls /state", "x.txt"),
        ("install", "IGEN"),
        ("IGEN", "install: kesz"),
        ("ls /bin", "captest.aox"),
        ("cat /boot.txt", "AO-OS ramdisk"),
        ("reboot", ""),
    ],
    [   # 3. menet: telepitett rendszer (az install ujraformazott: a regi x.txt nincs)
        ("mount", "aofs2"),
        ("cat /state/x.txt", "nincs ilyen"),
        ("write /state/x.txt telepites utan", "bajt"),
        ("ls /project", "(ures)"),
        ("write /project/README.txt agent projekt", "bajt"),
        ("mkdir /project/src", "AO> "),
        ("spawn /etc/agent-test.cap captest", "rc=0"),
        ("cat /project/src/a.txt", "nincs ilyen"),
        ("lastpanic", "nincs mentett panic"),
        ("cat /sys/version", "AO-OS 0.1-phase2"),
        ("reboot", ""),
    ],
    [   # 4. menet: perzisztencia a telepitett rendszeren
        ("cat /state/x.txt", "telepites utan"),
        ("cat /project/README.txt", "agent projekt"),
        ("ls /project/src", "(ures)"),
        ("run captest\n", "rc=0"),
    ],
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
    return buf.decode("latin-1", errors="replace"), needle == ""


def boot(t):
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
    return p, s


def echo_server():
    """TCP echo a host oldalon: a vendeg 10.0.2.2:4590-en eri el (QEMU user-net)."""
    import threading
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 4590))
    srv.listen(2)

    def loop():
        while True:
            try:
                c, _ = srv.accept()
            except OSError:
                return
            try:
                c.settimeout(5)
                data = b""
                while not data.endswith(b"\n"):
                    chunk = c.recv(256)
                    if not chunk:
                        break
                    data += chunk
                c.sendall(b"echo: " + data)
            except OSError:
                pass
            finally:
                c.close()
    th = threading.Thread(target=loop, daemon=True)
    th.start()
    return srv


def main():
    t = ao.tools()
    echo_server()
    # a teszt modositja a lemezkepet: masolaton dolgozunk
    img = os.path.join(ao.BUILD, "ao.img")
    test_img = os.path.join(ao.BUILD, "ao-test.img")
    shutil.copyfile(img, test_img)
    orig_cmd = ao.qemu_cmd

    def qemu_cmd_test(tools, extra):
        c = orig_cmd(tools, extra)
        return [x.replace(img, test_img) for x in c]
    ao.qemu_cmd = qemu_cmd_test

    ok = True
    log = []
    for si, cmds in enumerate(SESSIONS, 1):
        print(f"--- {si}. menet ---")
        p, s = boot(t)
        out, found = recv_until(s, "AO> ", 20)
        log.append(out)
        if not found:
            print(out)
            p.kill()
            print("FAIL  nem jott prompt")
            ok = False
            break
        for c, expect in cmds:
            s.sendall(c.encode("latin-1") + b"\n")
            out, found = recv_until(s, expect, 20 if c in ("install", "IGEN") else 8)
            log.append(out)
            if expect not in ("", "IGEN", "AO> "):
                rest, _ = recv_until(s, "AO> ", 3)
                log.append(rest)
            print(f"  {'OK ' if found else 'FAIL'}  {c.strip():40s} -> {expect!r}")
            ok &= found
        time.sleep(0.5)
        p.kill()
        s.close()
        time.sleep(0.5)
    full = "".join(log)
    with open(os.path.join(ao.BUILD, "smoke.log"), "w", encoding="latin-1", errors="replace") as f:
        f.write(full)
    for line in full.splitlines():
        if "boot -> prompt" in line or "100 sor" in line:
            print("  " + line.strip())
    print("SMOKE OK" if ok else "SMOKE FAIL (build/smoke.log)")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
