#!/usr/bin/env python3
"""AO-OS AI-hid: AOP v1 a netbook fele, Claude API (Anthropic SDK) a masik oldalon.

  python tools/bridge.py [--port 9010] [--model claude-sonnet-5] [--effort low|medium|high]

Alapertelmezes: claude-sonnet-5, kozepes erofeszites (gyors valasz, alacsony koltseg).
Megjegyzes: a "fast mode" csak az Opus-modelleken elerheto, a Sonneten nincs ilyen kapcsolo.

A kulcsot az ANTHROPIC_API_KEY kornyezeti valtozobol (vagy az `ant auth login` profilbol)
veszi az SDK. Protokoll: docs/AOP.md."""
import argparse
import os
import socket
import struct
import sys
import threading

import anthropic

MAGIC = b"AOP1"
HELLO, HELLO_OK, CONTEXT, PROMPT, DELTA, TOOL_CALL, TOOL_RESULT, END, ERR, PING, PONG = range(1, 12)

SYSTEM = """Te egy AI-agent vagy, aki egy AO-OS nevu, minimalis, sajat kernelu operacios rendszeren dolgozik
(Acer Aspire One netbook, 1366x768 szoveges konzol). Nincs Linux, nincs POSIX, nincs shell: nem letezik
sh, bash, busybox, mkdir, ls, cat vagy barmilyen parancssori program. Nincs internet.
Kizarolag a megadott eszkozokkel dolgozhatsz:
  fs_read(path), fs_write(path, content), fs_mkdir(path), fs_list(path), task_run(program, args),
  ask_user(question), done(summary).
Utvonalak: abszolutak, max 127 karakter (/project, /state, /tmp, /rd). Az fs_write a hianyzo szulo
konyvtarakat maga letrehozza. A task_run csak a /bin alatti AOX programokat inditja (pl. hello).
A capability-lista mutatja, mihez van jogod; E_CAP = nincs jogosultsag, ezt ne probald ujra.
Ha egy eszkoz hibat ad, olvasd el a magyarazatot, es ne ismeteld ugyanazt a hivast valtozatlanul.
Roviden, magyarul valaszolj, ekezetes betukkel. A feladat vegen hivd a done eszkozt egy rovid
osszefoglaloval."""

TOOLS = [
    {"name": "fs_read", "description": "Fajl tartalmanak beolvasasa (max 16 KiB).",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"],
                      "additionalProperties": False}, "strict": True},
    {"name": "fs_write", "description": "Fajl irasa (letrehozas vagy felulirasa).",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}, "content": {"type": "string"}},
                      "required": ["path", "content"], "additionalProperties": False}, "strict": True},
    {"name": "fs_mkdir", "description": "Konyvtar letrehozasa (a hianyzo szulokkel egyutt).",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"],
                      "additionalProperties": False}, "strict": True},
    {"name": "fs_list", "description": "Konyvtar tartalmanak listazasa.",
     "input_schema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"],
                      "additionalProperties": False}, "strict": True},
    {"name": "task_run", "description": "AOX program futtatasa az OS-en (pl. 'hello'), a kimenete a konzolra megy; a visszateresi kodot kapod.",
     "input_schema": {"type": "object", "properties": {"program": {"type": "string"}, "args": {"type": "string"}},
                      "required": ["program", "args"], "additionalProperties": False}, "strict": True},
    {"name": "ask_user", "description": "Kerdes a felhasznalonak a konzolon; a beirt sort kapod vissza.",
     "input_schema": {"type": "object", "properties": {"question": {"type": "string"}}, "required": ["question"],
                      "additionalProperties": False}, "strict": True},
    {"name": "done", "description": "A feladat befejezese rovid osszefoglaloval.",
     "input_schema": {"type": "object", "properties": {"summary": {"type": "string"}}, "required": ["summary"],
                      "additionalProperties": False}, "strict": True},
]


# ---------------------------------------------------------------- keretezes
def send_frame(sock, ftype, payload=b""):
    if isinstance(payload, str):
        payload = payload.encode("utf-8", errors="replace")
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
    ftype, _flags, length = struct.unpack("<HHI", hdr[4:])
    if length > 4 * 1024 * 1024:
        return None, None
    payload = recv_exact(sock, length) if length else b""
    if payload is None:
        return None, None
    return ftype, payload


def encode_tool_call(tool_id, name, inputs):
    out = f"{tool_id}\n{name}\n".encode()
    for k, v in inputs.items():
        vb = str(v).encode("utf-8", errors="replace")
        out += f"{k}\n{len(vb)}\n".encode() + vb + b"\n"
    return out


def parse_tool_result(payload):
    text = payload.decode("utf-8", errors="replace")
    parts = text.split("\n", 2)
    if len(parts) < 3:
        return parts[0] if parts else "", "error", "hianyos TOOL_RESULT"
    return parts[0], parts[1], parts[2]


# ---------------------------------------------------------------- egy kapcsolat
class Session:
    def __init__(self, sock, addr, model, effort):
        self.sock = sock
        self.addr = addr
        self.model = model
        self.effort = effort
        self.client = anthropic.Anthropic()
        self.messages = []
        self.context = ""
        self.agent = "agent"

    def log(self, msg):
        print(f"[{self.addr[0]} {self.agent}] {msg}", flush=True)

    def create_stream(self):
        kwargs = dict(model=self.model, max_tokens=16000, system=SYSTEM + "\n\n" + self.context,
                      tools=TOOLS, messages=self.messages,
                      output_config={"effort": self.effort})
        try:
            return self.client.beta.messages.stream(betas=["server-side-fallback-2026-07-01"],
                                                    fallbacks="default", **kwargs)
        except TypeError:
            return self.client.messages.stream(**kwargs)

    MAX_TOOL_CALLS = 40

    def run_turn(self, prompt):
        self.messages.append({"role": "user", "content": prompt})
        calls = 0
        while True:
            if calls >= self.MAX_TOOL_CALLS:
                send_frame(self.sock, ERR, f"tul sok eszkoz-hivas ({calls}), a feladat megszakitva")
                return
            with self.create_stream() as stream:
                for event in stream:
                    if event.type == "content_block_delta" and getattr(event.delta, "type", "") == "text_delta":
                        send_frame(self.sock, DELTA, event.delta.text)
                final = stream.get_final_message()
            self.messages.append({"role": "assistant", "content": final.content})
            if final.stop_reason == "refusal":
                send_frame(self.sock, ERR, "a modell elutasitotta a kerest")
                return
            if final.stop_reason != "tool_use":
                send_frame(self.sock, END, f"stop={final.stop_reason}\n")
                return
            tool_uses = [b for b in final.content if b.type == "tool_use"]
            results = []
            for tu in tool_uses:
                calls += 1
                self.log(f"tool {tu.name} {tu.input}")
                send_frame(self.sock, TOOL_CALL, encode_tool_call(tu.id, tu.name, tu.input))
                while True:
                    ftype, payload = recv_frame(self.sock)
                    if ftype is None:
                        raise ConnectionError("a kapcsolat megszakadt eszkoz-hivas kozben")
                    if ftype == PING:
                        send_frame(self.sock, PONG)
                        continue
                    if ftype != TOOL_RESULT:
                        continue
                    rid, status, content = parse_tool_result(payload)
                    if rid != tu.id:
                        continue
                    results.append({"type": "tool_result", "tool_use_id": tu.id, "content": content,
                                    "is_error": status != "ok"})
                    break
            self.messages.append({"role": "user", "content": results})
            if any(tu.name == "done" for tu in tool_uses):
                send_frame(self.sock, END, "stop=done\n")
                return

    def serve(self):
        self.log("kapcsolodott")
        try:
            while True:
                ftype, payload = recv_frame(self.sock)
                if ftype is None:
                    break
                if ftype == HELLO:
                    for line in payload.decode(errors="replace").splitlines():
                        if line.startswith("agent="):
                            self.agent = line[6:].strip() or "agent"
                    send_frame(self.sock, HELLO_OK, f"model={self.model}\n")
                elif ftype == CONTEXT:
                    self.context = payload.decode("utf-8", errors="replace")
                elif ftype == PROMPT:
                    text = payload.decode("utf-8", errors="replace")
                    self.log(f"prompt: {text[:80]!r}")
                    try:
                        self.run_turn(text)
                    except anthropic.APIStatusError as e:
                        send_frame(self.sock, ERR, f"API hiba {e.status_code}: {e.message}")
                    except anthropic.APIConnectionError:
                        send_frame(self.sock, ERR, "nem erheto el az API (halozat)")
                elif ftype == PING:
                    send_frame(self.sock, PONG)
        except (ConnectionError, OSError) as e:
            self.log(f"kapcsolat vege: {e}")
        finally:
            self.sock.close()
            self.log("lezarva")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9010)
    ap.add_argument("--model", default=os.environ.get("AO_MODEL", "claude-sonnet-5"))
    ap.add_argument("--effort", default=os.environ.get("AO_EFFORT", "medium"), choices=["low", "medium", "high"])
    a = ap.parse_args()
    if not os.environ.get("ANTHROPIC_API_KEY") and not os.environ.get("ANTHROPIC_AUTH_TOKEN"):
        print("figyelem: nincs ANTHROPIC_API_KEY; az SDK az `ant auth login` profilt probalja", flush=True)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", a.port))
    srv.listen(4)
    print(f"AO-OS hid: 0.0.0.0:{a.port}, modell {a.model}, erofeszites {a.effort}", flush=True)
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=Session(conn, addr, a.model, a.effort).serve, daemon=True).start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
