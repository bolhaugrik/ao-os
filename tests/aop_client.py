#!/usr/bin/env python3
"""AOP-kliens a PC-n: a netbook agentd-jet jatssza el, a hid tesztelesehez netbook nelkul.
  python tests/aop_client.py [--port 9010] [--psk-file ~/.ao-psk] "feladat szovege"
A fs_* eszkozoket egy memoria-beli mini fajlrendszerrel szolgalja ki, a task_run-t nem.
Ha a PSK-fajl letezik, titkositott csatornat ker (mint a netbook a /state/ai/psk-val)."""
import argparse
import os
import socket
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import aocrypto  # noqa: E402
from aop import (Conn, parse_tool_call, parse_kv, load_psk, default_psk_path,  # noqa: E402
                 HELLO, HELLO_OK, CONTEXT, PROMPT, DELTA, TOOL_CALL, TOOL_RESULT, END, ERR, PING, PONG)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9010)
    ap.add_argument("--psk-file", default=os.environ.get("AO_PSK_FILE", default_psk_path()))
    ap.add_argument("task", nargs="+")
    a = ap.parse_args()
    psk = load_psk(a.psk_file)
    files = {"/project/README.txt": "teszt projekt\n"}
    conn = Conn(socket.create_connection((a.host, a.port), timeout=600))
    cnonce = os.urandom(8)
    hello = "agent=pc-teszt\nversion=1\n" + (f"nonce={cnonce.hex()}\n" if psk else "")
    conn.send(HELLO, hello)
    ftype, payload = conn.recv()
    if ftype == ERR:
        sys.exit(f"[hid hiba: {payload.decode(errors='replace')}]")
    if ftype != HELLO_OK:
        sys.exit("varatlan valasz a kezfogasban")
    kv = parse_kv(payload)
    if psk:
        if kv.get("enc") != "1" or "nonce" not in kv:
            sys.exit("a kliensnek van PSK-ja, de a hid nem titkosit")
        conn.chan = aocrypto.Channel(psk, cnonce, bytes.fromhex(kv["nonce"]), is_server=False)
        print(f"[hid: model={kv.get('model')}, titkositott csatorna]")
    else:
        print(f"[hid: model={kv.get('model')}]")
    conn.send(CONTEXT, "Capability-lista (a kernel ezt kenyszeriti ki):\nagent pc-teszt\n  console\n  fs.read /project/**\n  fs.write /project/src/**\n")
    conn.send(PROMPT, " ".join(a.task))
    while True:
        ftype, payload = conn.recv()
        if ftype is None:
            print("\n[kapcsolat zarva vagy hibas keret]")
            break
        if ftype == DELTA:
            sys.stdout.write(payload.decode("utf-8", errors="replace"))
            sys.stdout.flush()
        elif ftype == TOOL_CALL:
            tid, name, args = parse_tool_call(payload)
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
            conn.send(TOOL_RESULT, f"{tid}\n{res[0]}\n{res[1]}")
        elif ftype == END:
            print(f"\n[vege: {payload.decode().strip()}]")
            break
        elif ftype == ERR:
            print(f"\n[hid hiba: {payload.decode(errors='replace')}]")
            break
        elif ftype == PING:
            conn.send(PONG)
    conn.close()
    print("fajlok:", {k: v[:40] for k, v in files.items()})


if __name__ == "__main__":
    main()
