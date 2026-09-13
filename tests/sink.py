#!/usr/bin/env python3
"""Nyers TCP-fogado a netbench parancshoz: fogad, mindent elolvas, kiirja a sebesseget.
  python tests/sink.py [port]   (alap 9020); a netbookon: netbench <PC IP> 9020 [MB]"""
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9020
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", port))
srv.listen(2)
print(f"sink: 0.0.0.0:{port}", flush=True)
while True:
    c, addr = srv.accept()
    t0 = time.time()
    total = 0
    first = None
    while True:
        data = c.recv(262144)
        if not data:
            break
        if first is None:
            first = time.time()
        total += len(data)
    t1 = time.time()
    dt = t1 - (first or t0)
    print(f"{addr[0]}: {total / 1024:.0f} KiB, {dt:.2f} s, {total / dt / 1000 if dt else 0:.0f} KB/s", flush=True)
    c.close()
