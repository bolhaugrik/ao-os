#!/usr/bin/env python3
"""AOP-kliens a PC-n: a netbook agentd-jet jatssza el, a hid tesztelesehez netbook nelkul.
  python tests/aop_client.py [--port 9010] "feladat szovege"
A fs_* eszkozoket egy memoria-beli mini fajlrendszerrel szolgalja ki, a task_run-t nem."""
import argparse
import socket
import struct
import sys

MAGIC = b"AOP1"
HELLO, HELLO_OK, CONTEXT, PROMPT, DELTA, TOOL_CALL, TOOL_RESULT, END, ERR, PING, PONG = range(1, 12)


def send_frame(sock, ftype, payload=b""):
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    sock.sendall(MAGIC + struct.pack("<HHI", ftype, 0, len(payload)) + payload)


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def recv_frame(sock):
    hdr = recv_exact(sock, 12)
    if hdr is None or hdr[:4] != MAGIC:
        return None, None
    ftype, _f, length = struct.unpack("<HHI", hdr[4:])
    return ftype, (recv_exact(sock, length) if length else b"")


def parse_call(payload):
    text = payload.decode("utf-8", errors="replace")
    tid, name, rest = text.split("\n", 2)
    args = {}
    pos = 0
    while pos < len(rest):
        nl = rest.find("\n", pos)
        if nl < 0:
            break
        key = rest[pos:nl]
        nl2 = rest.find("\n", nl + 1)
        length = int(rest[nl + 1:nl2])
        val = rest[nl2 + 1:nl2 + 1 + length]
        args[key] = val
        pos = nl2 + 1 + length + 1
    return tid, name, args


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9010)
    ap.add_argument("task", nargs="+")
    a = ap.parse_args()
    files = {"/project/README.txt": "teszt projekt\n"}
    s = socket.create_connection((a.host, a.port), timeout=600)
    send_frame(s, HELLO, "agent=pc-teszt\nversion=1\n")
    send_frame(s, CONTEXT, "Capability-lista (a kernel ezt kenyszeriti ki):\nagent pc-teszt\n  console\n  fs.read /project/**\n  fs.write /project/src/**\n")
    send_frame(s, PROMPT, " ".join(a.task))
    while True:
        ftype, payload = recv_frame(s)
        if ftype is None:
            print("\n[kapcsolat zarva]")
            break
        if ftype == HELLO_OK:
            print(f"[hid: {payload.decode().strip()}]")
        elif ftype == DELTA:
            sys.stdout.write(payload.decode("utf-8", errors="replace"))
            sys.stdout.flush()
        elif ftype == TOOL_CALL:
            tid, name, args = parse_call(payload)
            print(f"\n  [tool {name} {args}]")
            if name == "fs_write":
                p = args.get("path", "")
                if p.startswith("/project/src/"):
                    files[p] = args.get("content", "")
                    res = "ok", "ok, fajl irva"
                else:
                    res = "error", "E_CAP: nincs jogosultsag ehhez az utvonalhoz (lasd a capability-listat); ne probald ujra"
            elif name == "fs_read":
                p = args.get("path", "")
                res = ("ok", files[p]) if p in files else ("error", "E_NOENT: nincs ilyen fajl vagy konyvtar")
            elif name == "fs_list":
                p = args.get("path", "").rstrip("/") + "/"
                names = sorted({f[len(p):].split("/")[0] for f in files if f.startswith(p)})
                res = "ok", "\n".join(names) + "\n"
            elif name == "fs_mkdir":
                res = "ok", "ok, konyvtar letrehozva"
            elif name == "ask_user":
                res = "ok", input(f"? {args.get('question')}\n> ")
            elif name == "done":
                print(f"  [kesz: {args.get('summary')}]")
                res = "ok", "ok"
            else:
                res = "error", "E_NOENT: nincs ilyen program (task_run a PC-tesztben nem elerheto)"
            send_frame(s, TOOL_RESULT, f"{tid}\n{res[0]}\n{res[1]}")
        elif ftype == END:
            print(f"\n[vege: {payload.decode().strip()}]")
            break
        elif ftype == ERR:
            print(f"\n[hid hiba: {payload.decode(errors='replace')}]")
            break
        elif ftype == PING:
            send_frame(s, PONG)
    s.close()
    print("fajlok:", {k: v[:40] for k, v in files.items()})


if __name__ == "__main__":
    main()
