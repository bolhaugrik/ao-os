#!/usr/bin/env python3
"""Szimulalt AOP-hid a QEMU-tesztekhez (API-kulcs nelkul): fix forgatokonyv.
  python tests/fake_bridge.py [port] [psk-hex]
PSK-val: a nonce-t kuldo klienst titkositva szolgalja ki, a tobbit titkositatlanul (teszt-mod).
  PROMPT -> DELTA, fs_write /project/src/hello.txt, fs_read ugyanaz, fs_write /project/Makefile
  (elvart: E_CAP), fs_list /project, fs_write mely utvonal, done, END."""
import os
import socket
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
from aop import (Conn, encode_tool_call, parse_tool_result, server_handshake, parse_file, parse_kv,  # noqa: E402
                 HELLO, CONTEXT, PROMPT, DELTA, TOOL_RESULT, END, ERR, PING, PONG, FILE, CLIP_GET, CLIP,
                 PROJECT, IMPRINT)
import projector  # noqa: E402

BUILD = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build")

# a projector-teszt oldala: zaj (nav, script), cim, fejezetek, bekezdes, lista, idezet, kod, tablazat, linkek
SAMPLE_HTML = """<!doctype html><html><head><meta charset="utf-8"><title>Teszt lap cime</title>
<meta name="description" content="Rovid leiras a teszt laprol, a lenyomat note-csomopontja.">
<script>var zaj = 1;</script><style>.x{color:red}</style></head>
<body><nav><a href="/">Fooldal</a> <a href="/menu">Menu</a> <a href="/kapcsolat">Kapcsolat</a></nav>
<div class="cookie-banner">Ez az oldal sutiket hasznal, fogadd el.</div>
<main><h1>Elso fejezet</h1>
<p>Ez egy eleg hosszu bekezdes, hogy a kinyero megtartsa: a projector a weboldal tartalmat csupaszitja le,
es csak az informaciot adja at a netbooknak, kepek es stiluslapok nelkul. Az ekezetes betuk: árvíztűrő tükörfúrógép.</p>
<h2>Masodik fejezet</h2>
<ul><li>Elso elem a listaban</li><li>Masodik elem, kicsit hosszabb szoveggel</li><li>Harmadik elem</li></ul>
<blockquote>Egy idezet, amit kulon jelolunk a kepernyon.</blockquote>
<pre>int main(void) {
    return 0;
}</pre>
<table><tr><th>Nev</th><th>Ertek</th></tr><tr><td>boot</td><td>224 ms</td></tr><tr><td>ai szia</td><td>1,26 s</td></tr></table>
<p>Tovabbi olvasnivalo: <a href="/masodik-lap">a masodik teszt lap</a> es a <a href="https://example.com/kulso">kulso hivatkozas</a>.
Ez a bekezdes is eleg hosszu ahhoz, hogy megmaradjon a lenyomatban.</p>
</main><footer>Lablec, copyright, zaj.</footer></body></html>"""


def tool_call(conn, tid, name, **kw):
    conn.send(6, encode_tool_call(tid, name, kw))   # TOOL_CALL
    while True:
        ftype, payload = conn.recv()
        if ftype is None:
            raise ConnectionError("vege")
        if ftype == TOOL_RESULT:
            _rid, status, content = parse_tool_result(payload)
            return status, content


def handle(sock, addr, psk):
    conn = Conn(sock)
    print(f"{addr[0]}: kapcsolodott", flush=True)
    try:
        while True:
            ftype, payload = conn.recv()
            if ftype is None:
                break
            if ftype == HELLO:
                conn.chan = None
                _agent, err = server_handshake(conn, payload, psk, "fake", strict=False)
                print(f"{addr[0]}: {'titkositott' if conn.chan else 'titkositatlan'}", flush=True)
                if err:
                    break
            elif ftype == CONTEXT:
                print(f"context: {payload.decode(errors='replace')[:200]!r}", flush=True)
            elif ftype == PROMPT:
                text = payload.decode("utf-8", errors="replace")
                print(f"prompt: {text!r}", flush=True)
                conn.send(DELTA, "Szia, a szimulalt hid vagyok. ")
                st, _ = tool_call(conn, "t1", "fs_write", path="/project/src/hello.txt", content="irta az agent\n")
                conn.send(DELTA, f"iras: {st}. ")
                st, content = tool_call(conn, "t2", "fs_read", path="/project/src/hello.txt")
                conn.send(DELTA, f"olvasas: {st} [{content.strip()}]. ")
                st, err = tool_call(conn, "t3", "fs_write", path="/project/Makefile", content="tiltott\n")
                conn.send(DELTA, f"Makefile tiltott: {err.split(':')[0] if st != 'ok' else 'NEM TILTOTT'}. ")
                st, listing = tool_call(conn, "t4", "fs_list", path="/project")
                conn.send(DELTA, f"lista: {listing.strip().replace(chr(10), ',')}\n")
                st, _ = tool_call(conn, "t6", "fs_write", path="/project/src/mely/uj/a.txt", content="szulok letrehozva\n")
                conn.send(DELTA, f"mely iras: {st}\n")
                if conn.chan:
                    conn.send(DELTA, "csatorna: titkositott\n")
                tool_call(conn, "t5", "done", summary="kesz")
                conn.send(END, "stop=done\n")
            elif ftype == FILE:
                kind, name, content = parse_file(payload)
                text = content.decode("utf-8", errors="replace")
                if kind == "shot":
                    path = os.path.join(BUILD, f"shot-{name}.txt")
                    with open(path, "w", encoding="utf-8") as f:
                        f.write(text)
                    print(f"shot: {path} ({len(text)} karakter)", flush=True)
                    conn.send(DELTA, f"mentve: build/shot-{name}.txt")
                else:
                    print(f"clip: {text!r}", flush=True)
                    conn.send(DELTA, f"vagolapra masolva ({text.count(chr(10))} sor)")
                conn.send(END, "stop=file\n")
            elif ftype == CLIP_GET:
                conn.send(CLIP, "echo vagolap-ok\n")
            elif ftype == PROJECT:
                q = parse_kv(payload).get("q", "")
                print(f"projector: {q!r}", flush=True)
                if q == "hiba":
                    conn.send(ERR, "projector-teszt-hiba")
                else:
                    imp = projector.extract(SAMPLE_HTML, q if q.startswith("http") else "http://teszt.local/lap")
                    conn.send(IMPRINT, projector.to_json(imp))
            elif ftype == PING:
                conn.send(PONG)
    except (ConnectionError, OSError) as e:
        print(f"{addr[0]}: {e}", flush=True)
    finally:
        conn.close()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9010
    psk = bytes.fromhex(sys.argv[2]) if len(sys.argv) > 2 else None
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(4)
    print(f"szimulalt hid: 0.0.0.0:{port}{' PSK' if psk else ''}", flush=True)
    while True:
        c, addr = srv.accept()
        threading.Thread(target=handle, args=(c, addr, psk), daemon=True).start()


if __name__ == "__main__":
    main()
