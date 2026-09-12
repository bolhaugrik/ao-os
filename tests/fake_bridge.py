#!/usr/bin/env python3
"""Szimulalt AOP-hid a QEMU-tesztekhez (API-kulcs nelkul): fix forgatokonyv.
  PROMPT -> DELTA, fs_write /project/src/hello.txt, fs_read ugyanaz, fs_write /project/Makefile
  (elvart: E_CAP), fs_list /project, DELTA az eredmenyekkel, done, END."""
import socket
import struct
import sys
import threading

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
    payload = recv_exact(sock, length) if length else b""
    return ftype, payload


def tool_call(sock, tid, name, **kw):
    out = f"{tid}\n{name}\n".encode()
    for k, v in kw.items():
        vb = v.encode("utf-8")
        out += f"{k}\n{len(vb)}\n".encode() + vb + b"\n"
    send_frame(sock, TOOL_CALL, out)
    while True:
        ftype, payload = recv_frame(sock)
        if ftype is None:
            raise ConnectionError("vege")
        if ftype == TOOL_RESULT:
            parts = payload.decode("utf-8", errors="replace").split("\n", 2)
            return parts[1], parts[2] if len(parts) > 2 else ""


def handle(sock, addr):
    print(f"{addr[0]}: kapcsolodott", flush=True)
    try:
        while True:
            ftype, payload = recv_frame(sock)
            if ftype is None:
                break
            if ftype == HELLO:
                send_frame(sock, HELLO_OK, "model=fake\n")
            elif ftype == CONTEXT:
                print(f"context: {payload.decode(errors='replace')[:200]!r}", flush=True)
            elif ftype == PROMPT:
                text = payload.decode("utf-8", errors="replace")
                print(f"prompt: {text!r}", flush=True)
                send_frame(sock, DELTA, "Szia, a szimulalt hid vagyok. ")
                st, _ = tool_call(sock, "t1", "fs_write", path="/project/src/hello.txt", content="irta az agent\n")
                send_frame(sock, DELTA, f"iras: {st}. ")
                st, content = tool_call(sock, "t2", "fs_read", path="/project/src/hello.txt")
                send_frame(sock, DELTA, f"olvasas: {st} [{content.strip()}]. ")
                st, err = tool_call(sock, "t3", "fs_write", path="/project/Makefile", content="tiltott\n")
                send_frame(sock, DELTA, f"Makefile tiltott: {err.strip() if st != 'ok' else 'NEM TILTOTT'}. ")
                st, listing = tool_call(sock, "t4", "fs_list", path="/project")
                send_frame(sock, DELTA, f"lista: {listing.strip().replace(chr(10), ',')}\n")
                tool_call(sock, "t5", "done", summary="kesz")
                send_frame(sock, END, "stop=done\n")
            elif ftype == PING:
                send_frame(sock, PONG)
    except (ConnectionError, OSError) as e:
        print(f"{addr[0]}: {e}", flush=True)
    finally:
        sock.close()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9010
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(4)
    print(f"szimulalt hid: 0.0.0.0:{port}", flush=True)
    while True:
        c, addr = srv.accept()
        threading.Thread(target=handle, args=(c, addr), daemon=True).start()


if __name__ == "__main__":
    main()
