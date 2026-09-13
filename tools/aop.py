#!/usr/bin/env python3
"""AOP v1 keretezes es (opcionalis) PSK-titkositott csatorna a PC-oldali programoknak
(bridge.py, tests/fake_bridge.py, tests/aop_client.py). Protokoll: docs/AOP.md."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aocrypto  # noqa: E402

MAGIC = b"AOP1"
HELLO, HELLO_OK, CONTEXT, PROMPT, DELTA, TOOL_CALL, TOOL_RESULT, END, ERR, PING, PONG = range(1, 12)
FILE, CLIP_GET, CLIP = 12, 13, 14      # AOP 1.2: fajl a PC-re (shot/clip), PC-vagolap lekerese/valasza


def parse_file(payload):
    """FILE keret: 'kind\\nname\\n' + tartalom (bajtok) -> (kind, name, content)"""
    kind, name, rest = payload.split(b"\n", 2)
    return kind.decode(errors="replace"), name.decode(errors="replace"), rest
FLAG_ENC = 1


class Conn:
    """Egy TCP-kapcsolat AOP-keretekkel. chan != None utan minden keret titkositott."""

    def __init__(self, sock):
        self.sock = sock
        self.chan = None

    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def send(self, ftype, payload=b""):
        if isinstance(payload, str):
            payload = payload.encode("utf-8", errors="replace")
        if self.chan is not None:
            hdr = MAGIC + struct.pack("<HHI", ftype, FLAG_ENC, len(payload) + 16)
            self.sock.sendall(hdr + self.chan.seal(hdr, payload))
        else:
            self.sock.sendall(MAGIC + struct.pack("<HHI", ftype, 0, len(payload)) + payload)

    def recv(self):
        """(tipus, payload) vagy (None, None) ha zarva/hibas. Titkositott keretnel a tag
        ellenorzese utan a nyilt szoveget adja; rossz tag = (None, None)."""
        hdr = self._recv_exact(12)
        if hdr is None or hdr[:4] != MAGIC:
            return None, None
        ftype, flags, length = struct.unpack("<HHI", hdr[4:])
        if length > 4 * 1024 * 1024:
            return None, None
        payload = self._recv_exact(length) if length else b""
        if payload is None:
            return None, None
        if flags & FLAG_ENC:
            if self.chan is None:
                return None, None
            payload = self.chan.open(hdr, payload)
            if payload is None:
                return None, None
        elif self.chan is not None and ftype not in (ERR, HELLO):
            return None, None
        # nyilt HELLO titkositott csatornan: uj menet ugyanazon a TCP-folyamon (QEMU guestfwd
        # minden vendeg-kapcsolatot egy host-kapcsolatra fuz); a kezfogas ujra PSK-t kovetel
        return ftype, payload

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def encode_tool_call(tool_id, name, inputs):
    out = f"{tool_id}\n{name}\n".encode()
    for k, v in inputs.items():
        vb = str(v).encode("utf-8", errors="replace")
        out += f"{k}\n{len(vb)}\n".encode() + vb + b"\n"
    return out


def parse_tool_call(payload):
    """bajtokon dolgozik: a hossz-mezo bajtszam"""
    tid, name, rest = payload.split(b"\n", 2)
    args = {}
    pos = 0
    while pos < len(rest):
        nl = rest.find(b"\n", pos)
        if nl < 0:
            break
        key = rest[pos:nl].decode("utf-8", errors="replace")
        nl2 = rest.find(b"\n", nl + 1)
        length = int(rest[nl + 1:nl2])
        args[key] = rest[nl2 + 1:nl2 + 1 + length].decode("utf-8", errors="replace")
        pos = nl2 + 1 + length + 1
    return tid.decode(), name.decode(), args


def parse_tool_result(payload):
    text = payload.decode("utf-8", errors="replace")
    parts = text.split("\n", 2)
    if len(parts) < 3:
        return parts[0] if parts else "", "error", "hianyos TOOL_RESULT"
    return parts[0], parts[1], parts[2]


def parse_kv(payload):
    out = {}
    for line in payload.decode("utf-8", errors="replace").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def server_handshake(conn, hello_payload, psk, model, strict=True):
    """A szerver oldali HELLO-kezeles. Visszaad: (agent_nev, hiba_szoveg_vagy_None).
    psk: bytes vagy None. strict: PSK-val futo hid elutasitja a titkositatlan klienst."""
    kv = parse_kv(hello_payload)
    agent = kv.get("agent", "agent") or "agent"
    cnonce_hex = kv.get("nonce", "")
    if psk is not None and len(cnonce_hex) == 16:
        snonce = os.urandom(8)
        conn.send(HELLO_OK, f"model={model}\nnonce={snonce.hex()}\nenc=1\n")
        conn.chan = aocrypto.Channel(psk, bytes.fromhex(cnonce_hex), snonce, is_server=True)
        return agent, None
    if psk is not None and strict:
        conn.send(ERR, "a hid PSK-t var: ird a kulcsot a netbookon a /state/ai/psk fajlba")
        return agent, "kliens PSK nelkul, elutasitva"
    conn.send(HELLO_OK, f"model={model}\n")
    return agent, None


def load_psk(path):
    """PSK-fajl (64 hex karakter) beolvasasa; None, ha nincs."""
    if not path or not os.path.isfile(path):
        return None
    text = open(path, "r", encoding="utf-8").read().strip()
    if len(text) != 64:
        raise ValueError(f"a PSK-fajl 64 hex karakter kell legyen: {path}")
    return bytes.fromhex(text)


def default_psk_path():
    return os.path.join(os.path.expanduser("~"), ".ao-psk")
