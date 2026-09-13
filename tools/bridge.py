#!/usr/bin/env python3
"""AO-OS AI-hid: AOP v1 a netbook fele, valaszthato LLM-szolgaltato a masik oldalon.

  python tools/bridge.py [--provider claude|gemini] [--model NEV] [--effort low|medium|high] [--port 9010]

Szolgaltatok:
  claude  Anthropic SDK, kulcs: ANTHROPIC_API_KEY (vagy `ant auth login` profil). Alap: claude-sonnet-5.
  gemini  Google AI Studio REST (generativelanguage.googleapis.com), kulcs: GEMINI_API_KEY vagy
          GOOGLE_API_KEY. Alap: gemini-2.5-flash. Nincs kulon csomag, a beepitett urllib eleg.
A --provider elhagyhato: a modellnevbol (gemini-*) kovetkezik. Protokoll: docs/AOP.md.
Az OS-t a valasztas nem erinti: a hid mindket iranyban ugyanazt az AOP-t beszeli."""
import argparse
import json
import os
import socket
import struct
import sys
import threading
import urllib.error
import urllib.request


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

# szolgaltato-fuggetlen eszkozleiras: nev, leiras, parameterek (nev -> leiras), mind kotelezo
TOOL_DEFS = [
    ("fs_read", "Fajl tartalmanak beolvasasa (max 16 KiB).", {"path": "abszolut utvonal"}),
    ("fs_write", "Fajl irasa (letrehozas vagy felulirasa).", {"path": "abszolut utvonal", "content": "a fajl teljes tartalma"}),
    ("fs_mkdir", "Konyvtar letrehozasa (a hianyzo szulokkel egyutt).", {"path": "abszolut utvonal"}),
    ("fs_list", "Konyvtar tartalmanak listazasa.", {"path": "abszolut utvonal"}),
    ("task_run", "AOX program futtatasa az OS-en (pl. 'hello'); a kimenete a konzolra megy, a visszateresi kodot kapod.",
     {"program": "programnev a /bin alatt", "args": "argumentumok szokozzel elvalasztva (lehet ures)"}),
    ("ask_user", "Kerdes a felhasznalonak a konzolon; a beirt sort kapod vissza.", {"question": "a kerdes szovege"}),
    ("done", "A feladat befejezese rovid osszefoglaloval.", {"summary": "osszefoglalo"}),
]


# ---------------------------------------------------------------- keretezes (tools/aop.py)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aop import (Conn, encode_tool_call, parse_tool_result, server_handshake, load_psk, default_psk_path,  # noqa: E402
                 HELLO, HELLO_OK, CONTEXT, PROMPT, DELTA, TOOL_CALL, TOOL_RESULT, END, ERR, PING, PONG)
import aocrypto  # noqa: E402


# ---------------------------------------------------------------- szolgaltatok
class Backend:
    """Egy beszelgetes egy szolgaltatoval. run_turn: a prompt utan eszkozhivas-ciklus.
    call_tool(id, name, args) -> (status, content); send_delta(text); a visszateres a stop-ok."""
    MAX_TOOL_CALLS = 40

    def __init__(self, model, effort, system):
        self.model = model
        self.effort = effort
        self.system = system

    def run_turn(self, prompt, send_delta, call_tool, log):
        raise NotImplementedError


class ClaudeBackend(Backend):
    def __init__(self, model, effort, system):
        super().__init__(model, effort, system)
        import anthropic
        self.anthropic = anthropic
        self.client = anthropic.Anthropic()
        self.messages = []
        self.tools = [{
            "name": n, "description": d,
            "input_schema": {"type": "object", "properties": {k: {"type": "string", "description": v} for k, v in p.items()},
                             "required": list(p), "additionalProperties": False},
            "strict": True,
        } for n, d, p in TOOL_DEFS]

    def create_stream(self):
        kwargs = dict(model=self.model, max_tokens=16000, system=self.system, tools=self.tools,
                      messages=self.messages, output_config={"effort": self.effort})
        try:
            return self.client.beta.messages.stream(betas=["server-side-fallback-2026-07-01"],
                                                    fallbacks="default", **kwargs)
        except TypeError:
            return self.client.messages.stream(**kwargs)

    def run_turn(self, prompt, send_delta, call_tool, log):
        self.messages.append({"role": "user", "content": prompt})
        calls = 0
        while True:
            if calls >= self.MAX_TOOL_CALLS:
                return "toomany"
            with self.create_stream() as stream:
                for event in stream:
                    if event.type == "content_block_delta" and getattr(event.delta, "type", "") == "text_delta":
                        send_delta(event.delta.text)
                final = stream.get_final_message()
            self.messages.append({"role": "assistant", "content": final.content})
            if final.stop_reason == "refusal":
                return "refusal"
            if final.stop_reason != "tool_use":
                return final.stop_reason
            tool_uses = [b for b in final.content if b.type == "tool_use"]
            results = []
            for tu in tool_uses:
                calls += 1
                log(f"tool {tu.name} {tu.input}")
                status, content = call_tool(tu.id, tu.name, tu.input)
                results.append({"type": "tool_result", "tool_use_id": tu.id, "content": content,
                                "is_error": status != "ok"})
            self.messages.append({"role": "user", "content": results})
            if any(tu.name == "done" for tu in tool_uses):
                return "done"


class GeminiBackend(Backend):
    """Google AI Studio: streamGenerateContent SSE-vel, functionDeclarations eszkozokkel."""
    BASE = "https://generativelanguage.googleapis.com/v1beta/models"

    def __init__(self, model, effort, system):
        super().__init__(model, effort, system)
        self.key = os.environ.get("GEMINI_API_KEY") or os.environ.get("GOOGLE_API_KEY")
        if not self.key:
            raise RuntimeError("nincs GEMINI_API_KEY (vagy GOOGLE_API_KEY) kornyezeti valtozo")
        self.contents = []
        self.call_seq = 0
        self.tools = [{"functionDeclarations": [{
            "name": n, "description": d,
            "parameters": {"type": "OBJECT",
                           "properties": {k: {"type": "STRING", "description": v} for k, v in p.items()},
                           "required": list(p)},
        } for n, d, p in TOOL_DEFS]}]

    def request(self, send_delta):
        body = {
            "systemInstruction": {"parts": [{"text": self.system}]},
            "contents": self.contents,
            "tools": self.tools,
            "generationConfig": {"maxOutputTokens": 8192, "temperature": 0.2},
        }
        if self.effort == "low":
            body["generationConfig"]["thinkingConfig"] = {"thinkingBudget": 0}
        url = f"{self.BASE}/{self.model}:streamGenerateContent?alt=sse"
        req = urllib.request.Request(url, data=json.dumps(body).encode("utf-8"),
                                     headers={"Content-Type": "application/json", "x-goog-api-key": self.key})
        text_parts = []
        calls = []
        finish = "STOP"
        with urllib.request.urlopen(req, timeout=300) as resp:
            for raw in resp:
                line = raw.decode("utf-8", errors="replace").strip()
                if not line.startswith("data:"):
                    continue
                chunk = json.loads(line[5:].strip())
                for cand in chunk.get("candidates", []):
                    finish = cand.get("finishReason", finish) or finish
                    for part in cand.get("content", {}).get("parts", []):
                        if "text" in part and part["text"]:
                            text_parts.append(part["text"])
                            send_delta(part["text"])
                        if "functionCall" in part:
                            fc = part["functionCall"]
                            calls.append((fc.get("name", ""), fc.get("args", {}) or {}))
                if "error" in chunk:
                    raise RuntimeError(chunk["error"].get("message", "ismeretlen hiba"))
        return "".join(text_parts), calls, finish

    def run_turn(self, prompt, send_delta, call_tool, log):
        self.contents.append({"role": "user", "parts": [{"text": prompt}]})
        ncalls = 0
        while True:
            if ncalls >= self.MAX_TOOL_CALLS:
                return "toomany"
            try:
                text, calls, finish = self.request(send_delta)
            except urllib.error.HTTPError as e:
                detail = e.read().decode("utf-8", errors="replace")[:300]
                raise RuntimeError(f"Gemini API hiba {e.code}: {detail}")
            model_parts = []
            if text:
                model_parts.append({"text": text})
            for name, args in calls:
                model_parts.append({"functionCall": {"name": name, "args": args}})
            if not model_parts:
                model_parts.append({"text": ""})
            self.contents.append({"role": "model", "parts": model_parts})
            if finish == "SAFETY":
                return "refusal"
            if not calls:
                return "end_turn"
            responses = []
            for name, args in calls:
                ncalls += 1
                self.call_seq += 1
                tid = f"g{self.call_seq}"
                log(f"tool {name} {args}")
                status, content = call_tool(tid, name, {k: str(v) for k, v in args.items()})
                responses.append({"functionResponse": {"name": name, "response": {
                    "status": status, "result": content}}})
            self.contents.append({"role": "user", "parts": responses})
            if any(name == "done" for name, _ in calls):
                return "done"


def make_backend(provider, model, effort, system):
    if provider == "gemini":
        return GeminiBackend(model, effort, system)
    return ClaudeBackend(model, effort, system)


# ---------------------------------------------------------------- egy kapcsolat
class Session:
    def __init__(self, sock, addr, provider, model, effort, psk, strict):
        self.conn = Conn(sock)
        self.addr = addr
        self.provider = provider
        self.model = model
        self.effort = effort
        self.psk = psk
        self.strict = strict
        self.backend = None
        self.context = ""
        self.agent = "agent"

    def log(self, msg):
        print(f"[{self.addr[0]} {self.agent}] {msg}", flush=True)

    def call_tool(self, tool_id, name, inputs):
        self.conn.send(TOOL_CALL, encode_tool_call(tool_id, name, inputs))
        while True:
            ftype, payload = self.conn.recv()
            if ftype is None:
                raise ConnectionError("a kapcsolat megszakadt eszkoz-hivas kozben")
            if ftype == PING:
                self.conn.send(PONG)
                continue
            if ftype != TOOL_RESULT:
                continue
            rid, status, content = parse_tool_result(payload)
            if rid != tool_id:
                continue
            return status, content

    def run_turn(self, prompt):
        if self.backend is None:
            self.backend = make_backend(self.provider, self.model, self.effort, SYSTEM + "\n\n" + self.context)
        stop = self.backend.run_turn(prompt, lambda t: self.conn.send(DELTA, t), self.call_tool, self.log)
        if stop == "refusal":
            self.conn.send(ERR, "a modell elutasitotta a kerest")
        elif stop == "toomany":
            self.conn.send(ERR, f"tul sok eszkoz-hivas ({Backend.MAX_TOOL_CALLS}), a feladat megszakitva")
        else:
            self.conn.send(END, f"stop={stop}\n")

    def serve(self):
        self.log("kapcsolodott")
        try:
            while True:
                ftype, payload = self.conn.recv()
                if ftype is None:
                    break
                if ftype == HELLO:
                    self.agent, err = server_handshake(self.conn, payload, self.psk, self.model, self.strict)
                    if err:
                        self.log(err)
                        break
                    self.log("titkositott csatorna" if self.conn.chan else "titkositatlan kapcsolat")
                elif ftype == CONTEXT:
                    self.context = payload.decode("utf-8", errors="replace")
                elif ftype == PROMPT:
                    text = payload.decode("utf-8", errors="replace")
                    self.log(f"prompt: {text[:80]!r}")
                    try:
                        self.run_turn(text)
                    except (RuntimeError, urllib.error.URLError) as e:
                        self.conn.send(ERR, f"{e}")
                    except Exception as e:  # SDK-hibak (APIStatusError, APIConnectionError, ...)
                        name = type(e).__name__
                        msg = getattr(e, "message", None) or str(e)
                        self.conn.send(ERR, f"{name}: {msg[:300]}")
                elif ftype == PING:
                    self.conn.send(PONG)
        except (ConnectionError, OSError) as e:
            self.log(f"kapcsolat vege: {e}")
        finally:
            self.conn.close()
            self.log("lezarva")


def gemini_list_models():
    key = os.environ.get("GEMINI_API_KEY") or os.environ.get("GOOGLE_API_KEY")
    if not key:
        sys.exit("hiba: nincs GEMINI_API_KEY (vagy GOOGLE_API_KEY) kornyezeti valtozo")
    req = urllib.request.Request(f"{GeminiBackend.BASE}?pageSize=100", headers={"x-goog-api-key": key})
    with urllib.request.urlopen(req, timeout=60) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    for m in data.get("models", []):
        if "generateContent" in m.get("supportedGenerationMethods", []):
            print(f"  {m['name'].replace('models/', ''):40s} {m.get('displayName', '')}")


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9010)
    ap.add_argument("--list-models", action="store_true", help="Gemini: elerheto modellek listaja, majd kilep")
    ap.add_argument("--provider", choices=["claude", "gemini"], default=os.environ.get("AO_PROVIDER"))
    ap.add_argument("--model", default=os.environ.get("AO_MODEL"))
    ap.add_argument("--effort", default=os.environ.get("AO_EFFORT", "medium"), choices=["low", "medium", "high"])
    ap.add_argument("--psk-file", default=os.environ.get("AO_PSK_FILE", default_psk_path()),
                    help="PSK (64 hex) fajlja; alap: ~/.ao-psk. Ha letezik, a hid titkositva beszel.")
    ap.add_argument("--gen-psk", action="store_true", help="uj PSK generalasa a --psk-file helyre, majd kilep")
    ap.add_argument("--psk-optional", action="store_true", help="PSK nelkuli klienst is fogad (csak tesztre)")
    a = ap.parse_args()
    if a.list_models:
        gemini_list_models()
        return
    if a.gen_psk:
        if os.path.exists(a.psk_file):
            sys.exit(f"mar van PSK: {a.psk_file} (torold, ha ujat akarsz)")
        hexkey = os.urandom(32).hex()
        with open(a.psk_file, "w", encoding="utf-8") as f:
            f.write(hexkey + "\n")
        print(f"uj PSK: {a.psk_file}")
        print("a netbookon (egyszer):")
        print(f"  mkdir /state/ai")
        print(f"  write /state/ai/psk {hexkey}")
        return
    psk = load_psk(a.psk_file)
    if not a.provider:
        a.provider = "gemini" if (a.model or "").startswith("gemini") else "claude"
    if not a.model:
        a.model = "gemini-2.5-flash" if a.provider == "gemini" else "claude-sonnet-5"
    if a.provider == "claude":
        if not os.environ.get("ANTHROPIC_API_KEY") and not os.environ.get("ANTHROPIC_AUTH_TOKEN"):
            print("figyelem: nincs ANTHROPIC_API_KEY; az SDK az `ant auth login` profilt probalja", flush=True)
    else:
        if not os.environ.get("GEMINI_API_KEY") and not os.environ.get("GOOGLE_API_KEY"):
            sys.exit("hiba: nincs GEMINI_API_KEY (vagy GOOGLE_API_KEY) kornyezeti valtozo")
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", a.port))
    srv.listen(4)
    print(f"AO-OS hid: 0.0.0.0:{a.port}, szolgaltato {a.provider}, modell {a.model}, erofeszites {a.effort}, "
          f"{'PSK-titkositas' if psk else 'titkositatlan (nincs ' + a.psk_file + ')'}", flush=True)
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=Session(conn, addr, a.provider, a.model, a.effort, psk, not a.psk_optional).serve,
                         daemon=True).start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
