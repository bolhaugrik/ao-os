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
TEST_PSK = "0f1e2d3c4b5a69788796a5b4c3d2e1f00112233445566778899aabbccddeeff0"


def _build_stamp():
    """a build belyege (rootfs/etc/build, az ao.py build irja); a halozati update tesztje ezt varja vissza"""
    try:
        with open(os.path.join(ROOT, "rootfs", "etc", "build"), encoding="ascii") as f:
            return f.readline().strip()
    except OSError:
        return "?"


BUILD_STAMP = _build_stamp()

# (parancs, elvart reszlet a kimenetben); a "\n" a parancs vegen extra Entert kuld
SESSIONS = [
    [   # 1. menet: alap parancsok, taskok, capability, formazas, perzisztens iras, panic
        ("help", "Tab = kiegeszites"),
        ("help copy", "vagolap"),
        ("head --help", "elso / utolso N sor"),
        ("grep -h", "sorok keresese"),
        ("hel\t", "Rendszer"),
        ("cat /boot.t\t", "AO-OS ramdisk"),
        ("status", "hid"),
        ("time", ":"),
        ("date", "-"),
        ("mem", "hasznalhato:"),
        ("cpu", "tsc:"),
        ("disk", "AO-particio"),
        ("ls", "boot.txt"),
        ("cat boot.txt", "AO-OS ramdisk"),
        ("mount", "ramfs"),
        ("run hello x y\n", "rc=0"),
        ("run fault", "rc=-17 (kivetel)"),
        ("ps", "shell"),
        ("run keytest", "nyers mod"),
        ("ab\x1b[Aq", "esemeny"),                 # a, b, Fel, q (+ az Enter, ha egy olvasasba esik)
        ("echo szoveges-mod-vissza", "szoveges-mod-vissza"),
        ("run fbtest\n", "fbtest: ok"),
        ("run fputest", "fputest: ok"),
        ("run malloctest", "malloctest: ok"),
        ("spawn /etc/agents/clip.cap malloctest 6", "korlat ok"),   # 4M korlat: a foglalo NULL-t kap, nem omlik ossze
        ("run cryptotest", "cryptotest: minden OK"),
        ("spawn /etc/agents/clip.cap fbtest", "fbtest: fb: E_CAP"),
        ("echo konzol-vissza", "konzol-vissza"),
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
        ("cat /sys/version", "AO-OS 0.2"),
        ("append /state/rc echo rc-fut-ok", "bajt"),
        ("append /state/rc kbd hu", "bajt"),
        ("cat /state/rc", "kbd hu"),
        # fajl-segedprogramok
        ("cp /state/x.txt /state/y.txt", "15 bajt"),
        ("cat /state/y.txt", "telepites utan"),
        ("mv /state/y.txt /state/z.txt", "15 bajt"),
        ("cat /state/y.txt", "nincs ilyen"),
        ("cp /state/z.txt /project", "/project/z.txt, 15 bajt"),
        ("wc /state/z.txt", "1 sor  2 szo  15 bajt"),
        ("head /state/rc 1", "echo rc-fut-ok"),
        ("tail /state/rc 1", "kbd hu"),
        ("grep utan /state/z.txt", "1: telepites utan"),
        ("grep -r UTAN /state", "nincs talalat"),
        ("grep -i -r UTAN /state", "/state/z.txt:1: telepites utan"),
        ("hexdump /state/z.txt", "74 65 6c 65 70"),
        ("stat /state/z.txt", "fajl, 15 bajt"),
        ("find /state z*", "/state/z.txt  15 B"),
        ("find *.txt", "/state/x.txt"),
        ("du /state", "osszesen"),
        ("df", "KiB szabad"),
        ("tree /state", "z.txt  15 B"),
        ("more /state/rc", "kbd hu"),
        # szerkeszto: ket sor, mentes (^S), kilepes (^Q ketszer: a Enter utan mar modositott)
        ("edit /state/e.txt", "uj fajl"),
        ("elso sor", ""),
        ("masodik sor\x13", "mentve: /state/e.txt"),
        ("\x11\x11", "edit: /state/e.txt, 3 sor"),
        ("cat /state/e.txt", "masodik sor"),
        ("wc /state/e.txt", "2 sor  4 szo"),
        ("rm /state/z.txt", "AO> "),
        ("rm /project/z.txt", "AO> "),
        ("reboot", ""),
    ],
    [   # 4. menet: perzisztencia a telepitett rendszeren + AI-agent a szimulalt hiddal
        ("", "rc-fut-ok"),              # a boot-kimenetben: az indito szkript lefutott
        ("", "kiosztas: hu"),
        ("kbd us", "kiosztas: us"),
        ("cat /state/x.txt", "telepites utan"),
        ("cat /project/README.txt", "agent projekt"),
        ("ls /project/src", "(ures)"),
        ("run captest\n", "rc=0"),
        ("mkdir /state/agents/coder", "AO> "),
        ("agent coder irj egy fajlt a projektbe", "lista: README.txt,src/"),
        ("cat /project/src/hello.txt", "irta az agent"),
        ("cat /project/src/mely/uj/a.txt", "szulok letrehozva"),
        ("cat /state/agents/coder/context.txt", "## feladat"),
        ("audit", "fs.write  ELUTASITVA  /project/Makefile"),
        ("ai szia", "[kesz: kesz]"),
        ("update rd", "IGEN"),                # pendrive-os ut: a futo ramdiskbol
        ("IGEN", "update: kesz"),
        ("cat /state/x.txt", "telepites utan"),
        ("cat /project/src/hello.txt", "irta az agent"),
        ("echo árvíztűrő", "árvíztűrő"),
        ("run cryptotest", "cryptotest: minden OK"),
        ("mkdir /state/ai", "AO> "),
        (f"write /state/ai/psk {TEST_PSK}", "bajt"),
        ("rm /project/src/hello.txt", "AO> "),
        ("agent coder titkositott proba", "csatorna: titkositott"),
        ("cat /project/src/hello.txt", "irta az agent"),
        ("shot proba", "mentve: build/shot-proba.txt"),
        ("screenshot", "[kepernyokep: /state/shots/1.ppm]"),
        ("shot /state/shots/1.ppm", "mentve: build/shot-1.ppm.txt"),
        ("shot /state/x.txt", "mentve: build/shot-x.txt.txt"),
        ("rm /state/shots/1.ppm", "AO> "),                        # 2,9 MB: a 7 MiB-os tesztlemezen kell a hely
        ("shot /etc/motd", "E_CAP"),                              # a clip manifest az /etc-t nem olvashatja
        ("echo masolando szoveg", "masolando szoveg"),
        ("copy", "vagolapra masolva (1 sor)"),
        ("copy 3", "vagolapra masolva (3 sor)"),
        ("paste /tmp/p.txt", "bajt"),
        ("cat /tmp/p.txt", "echo vagolap-ok"),
        ("paste", "paste: 16 bajt"),
        ("\r", "vagolap-ok"),                 # a parancssorba toltott sor futtatasa (Enter)
        ("projector --dump http://teszt.local/lap", "[item 4] Masodik elem"),
        ("projector --dump hiba", "[hid hiba: projector-teszt-hiba]"),
        ("projector --dump --view http://teszt.local/lap", "3 link, ellenorzo osszeg ok"),   # RENDER + inflate + fb
        ("projector http://teszt.local/lap", "PROJECTOR"),
        ("\x1b[B\x1b[Bs", "mentve: /state/projector/teszt-lap-cime.json"),
        ("q", "projector: Teszt lap cime ("),
        ("cat /state/projector/teszt-lap-cime.json", "\"t\":\"quote\""),
        ("fetch szoveg.txt", "/state/inbox/szoveg.txt: 16 bajt"),
        ("cat /state/inbox/szoveg.txt", "fetch-teszt sor"),
        ("mkdir /state/games", "AO> "),
        ("fetch nagy.bin /state/games", "/state/games/nagy.bin: 200000 bajt"),
        ("ls /state/games", "nagy.bin"),
        ("fetch nincs.txt", "nincs ilyen fajl a share/"),
        ("fetch oriasi.bin /state/games", "/state/games/oriasi.bin: 4400000 bajt"),   # AOFS2 ketszeres indirekt
        ("ls /state/games", "4400000 B"),
        ("fetch doom.aox /state/games", "doom.aox: "),          # a share/ mappabol (a build teszi oda)
        ("doom", "nincs WAD a /state/games alatt"),               # program van, WAD nincs: ertheto uzenet
        ("rm /state/games/oriasi.bin", "AO> "),
        ("sync", "sync: ok"),
        # MicroPython: telepites a share/-bol, egysoros, szkript, modul importja, REPL fajlmuveletekkel
        ("mkdir /state/bin", "AO> "),
        ("fetch python.aox /state/bin", "python.aox: "),
        ("python -c print(6*7)", "42"),
        ("write /state/t.py print('fajl-ok', 2**100)", "bajt"),
        ("python /state/t.py", "fajl-ok 1267650600228229401496703205376"),
        ("mkdir /state/lib", "AO> "),
        ("write /state/lib/modul.py X = 7", "bajt"),
        ("python -c import modul;print(modul.X*6)", "42"),
        ("python", ">>> "),
        ("print('szia', 1/4)", "szia 0.25"),
        ("import os; print(sorted(os.listdir('/state/lib')))", "['modul.py']"),
        ("f=open('/state/w.txt','w'); f.write('irt'); f.close(); print(open('/state/w.txt').read())", "irt"),
        ("\x04", "AO> "),
        ("cat /state/w.txt", "irt"),
        # vagolap a PC-rol: Ctrl+V a shellben (billentyuleutesek), a szerkesztoben (nyers beillesztes), ^K -> PC
        ("\x16", "vagolap-ok"),                       # a szimulalt hid vagolapja "echo vagolap-ok\n": lefut
        ("edit /state/p.txt", "uj fajl"),
        ("\x16", "beillesztve: 1 sor"),
        ("\x0b", "vagolapra masolva"),                # ^K: a sor a PC vagolapjara (a hid valasza)
        ("\x13", "mentve: /state/p.txt"),
        ("\x11\x11", "edit: /state/p.txt"),
        ("cat /state/p.txt", "echo vagolap-ok"),
        ("2048", "legjobb"),
        ("\x1b[A\x1b[D\x1b[B\x1b[Cq", "2048: pont"),
        ("cat /state/games/2048", "AO> "),
        # halozati update: a share/boot.img a szimulalt hidrol; ugyanaz a build -> nem ir; force -> ir es ujraindit
        ("write /etc/build regi-build", "bajt"),
        ("update", "mar ez a build fut"),
        ("update force", "ujrainditas 3 mp"),
    ],
    [   # 5. menet: a halozati update utan az elso boot a ramdiskbol frissiti a bin/-t es etc/-t
        ("", "[frissitve: bin/ es etc/ a ramdiskbol"),
        ("", "build " + BUILD_STAMP),
        ("cat /etc/build", BUILD_STAMP),
        ("cat /state/x.txt", "telepites utan"),
        ("ls /state/inbox", "szoveg.txt"),
        ("run hello\n", "rc=0"),
        ("update", "mar ez a build fut"),
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
        if needle.encode("utf-8") in buf:
            return buf.decode("utf-8", errors="replace"), True
    return buf.decode("utf-8", errors="replace"), needle == ""


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
    # szimulalt AOP-hid a 9011-es porton; a vendeg a 10.0.2.100:9010 cimen eri el (guestfwd),
    # igy a valodi hid (9010) mellett is futhat a teszt
    ao.QEMU_GUESTFWD = "tcp:10.0.2.100:9010-tcp:127.0.0.1:9011"
    bridge = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "fake_bridge.py"), "9011", TEST_PSK],
                              cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    import atexit
    atexit.register(bridge.kill)
    time.sleep(0.5)
    if bridge.poll() is not None:
        sys.exit("a szimulalt hid nem indult (9011-es port foglalt?)")
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
        boot_out = out
        for c, expect in cmds:
            if c == "":                                     # ellenorzes a boot-kimeneten
                found = expect in boot_out
                print(f"  {'OK ' if found else 'FAIL'}  {'(boot)':40s} -> {expect!r}")
                ok &= found
                continue
            s.sendall(c.encode("utf-8") + b"\n")
            out, found = recv_until(s, expect, 90 if c.startswith(("install", "IGEN", "fetch", "shot /", "update")) else 8)
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
    with open(os.path.join(ao.BUILD, "smoke.log"), "w", encoding="utf-8", errors="replace") as f:
        f.write(full)
    for line in full.splitlines():
        if "boot -> prompt" in line or "100 sor" in line:
            print("  " + line.strip())
    print("SMOKE OK" if ok else "SMOKE FAIL (build/smoke.log)")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
