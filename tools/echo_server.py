#!/usr/bin/env python3
"""TCP echo-szerver a halozati teszthez: a netbook `nc <PC-IP> 4590 szoveg` parancsa
"echo: szoveg" valaszt kap. Minden LAN-cimen figyel (0.0.0.0:4590)."""
import socket
import sys
import threading

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4590


def handle(c, addr):
    try:
        c.settimeout(10)
        data = b""
        while not data.endswith(b"\n"):
            chunk = c.recv(256)
            if not chunk:
                break
            data += chunk
        print(f"{addr[0]}: {data.decode('latin-1').strip()}", flush=True)
        c.sendall(b"echo: " + data)
    except OSError as e:
        print(f"{addr[0]}: hiba {e}", flush=True)
    finally:
        c.close()


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", PORT))
    srv.listen(4)
    print(f"echo-szerver: 0.0.0.0:{PORT}", flush=True)
    while True:
        c, addr = srv.accept()
        threading.Thread(target=handle, args=(c, addr), daemon=True).start()


if __name__ == "__main__":
    main()
