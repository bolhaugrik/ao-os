# AOP v1 – AO-OS ↔ híd protokoll

A netbook nem beszél HTTPS-t és JSON-t. Egy TCP-kapcsolaton keretezett üzeneteket cserél a híddal
(`tools/bridge.py`), amely a tényleges AI-API-hoz beszél. A híd cseréje nem érinti az OS-t.

## Szolgáltatók a híd mögött

A híd (`tools/bridge.py`) `--provider claude|gemini` kapcsolóval választ: Claude az Anthropic SDK-n át
(`ANTHROPIC_API_KEY`, alap `claude-sonnet-5`), Gemini a Google AI Studio REST-API-ján át
(`GEMINI_API_KEY`, alap `gemini-2.5-flash`, `--list-models` az elérhető nevekhez). A netbook felé
mindkettő ugyanezt az AOP-t beszéli, az OS-en semmi nem függ a szolgáltatótól.

## Titkosítás (PSK, AOP 1.1)

Előre megosztott 32 bájtos kulcs: a PC-n `~/.ao-psk` (`python tools/bridge.py --gen-psk` készíti),
a netbookon `/state/ai/psk` (64 hex karakter). Ha mindkét oldalon van kulcs:

1. A kliens `HELLO`-ja `nonce=<8 bájt hex>` sort is tartalmaz (TSC + tick + pid ChaCha20-szal keverve).
2. A híd `HELLO_OK`-ja `nonce=<8 bájt hex>` (os.urandom) és `enc=1` sort ad.
3. Alkulcs = HChaCha20(psk, kliens_nonce ‖ híd_nonce), tehát kapcsolatonként más kulcs.
4. Innentől minden keret: `flags` 0. bit = 1, payload = ChaCha20-Poly1305(ciphertext ‖ 16 bájt tag),
   nonce = irány (`cli\0` vagy `srv\0`) ‖ 64 bites számláló, AAD = a 12 bájtos fejléc.
   A számláló irányonként szigorúan növekszik: az ismételt vagy sorrenden kívüli keret elutasítva.

PSK-val futó híd a kulcs nélküli klienst `ERR`-rel utasítja el; kulccsal futó kliens a titkosítatlan
hidat nem fogadja el. Implementáció: `user/crypto.c` (RFC 8439, tesztvektorokkal: `run cryptotest`)
és `tools/aocrypto.py` (`--selftest`). A kulcsot a netbook sosem küldi el; a híd API-kulcsa a PC-n marad.

## Keret

```
u8[4]  magic   "AOP1"
u16    type
u16    flags   (0)
u32    len     payload hossza
u8[len] payload
```

Minden szám little-endian.

## Típusok

| # | Név | Irány | Payload |
|---|-----|-------|---------|
| 1 | HELLO | OS → híd | `agent=NÉV\nversion=1\n` |
| 2 | HELLO_OK | híd → OS | `model=...\n` |
| 3 | CONTEXT | OS → híd | szöveg: capability-lista és az agent mentett kontextusa |
| 4 | PROMPT | OS → híd | a feladat szövege |
| 5 | DELTA | híd → OS | szövegdarab (streamelt válasz) |
| 6 | TOOL_CALL | híd → OS | `id\nnév\n` majd ismétlődve `kulcs\n<hossz>\n<érték>\n` |
| 7 | TOOL_RESULT | OS → híd | `id\nok|error\n` + tartalom |
| 8 | END | híd → OS | `stop=end_turn\n` |
| 9 | ERR | híd → OS | hibaszöveg |
| 10 | PING | bármely | üres |
| 11 | PONG | bármely | üres |
| 12 | FILE | OS → híd | `kind\nnév\n` + tartalom; `kind` = `shot` (a híd `shots/` mappájába menti) vagy `clip` (a PC vágólapjára); válasz DELTA + END |
| 13 | CLIP_GET | OS → híd | üres; a PC vágólapját kéri |
| 14 | CLIP | híd → OS | a vágólap szövege (UTF-8) |

| 15 | PROJECT | OS → híd | `q=<cím vagy kérdés>\ndepth=<n>\n` |
| 16 | IMPRINT | híd → OS | a lenyomat JSON-ban (lásd lent), legfeljebb 60 000 bájt |
| 17 | FETCH | OS → híd | `name=<fájlnév>\n`; a híd a `share/` mappa fájlját FILE-keretekben (`data\nnév\n` + darab, ≤ 60 000 bájt) küldi, majd END `stop=fetch\nsize=N\n`; hiba: ERR |

A 12–14 típusok (AOP 1.2) a `shot`, `copy`, `paste` parancsokat szolgálják: az `agentd` `--file` / `--clip`
módban, a `clip` manifesttel (`/etc/agents/clip.cap`) küldi őket, a kézfogás és a titkosítás ugyanaz.
A híd a PING-re a kézfogás előtt is PONG-gal felel: ezzel ellenőrzi a netbook indításkor, hogy a híd elérhető-e.

## Lenyomat (AOP 1.3, `projector`)

A híd (`tools/projector.py`) letölti és lecsupaszítja az oldalt, a netbook (`user/projector.c`) csak ezt a
lapos JSON-t kapja és rendezi el. A séma szándékosan lapos: egy ~200 soros részhalmaz-elemző (`user/json.c`) elég hozzá.

```json
{"v":1,"q":"https://…","title":"…",
 "sources":[{"id":1,"url":"…","title":"…"}],
 "nodes":[{"t":"title","w":9,"s":1,"x":"…"},
          {"t":"head","w":7,"s":1,"x":"…"},
          {"t":"para","w":5,"s":1,"x":"…"},
          {"t":"link","w":2,"s":1,"x":"szöveg","to":"https://…"}]}
```

`t` (típus): `title`, `head` (w 8 = h1 … 5 = h4+), `para`, `item` (listaelem), `quote`, `code`, `link` (`to` a cél),
`row` (táblázatsor, cellák `|`-lel), `note` (meta-leírás), `fact` (AI-tól, 4.3). `w` súly 0–9 (3 alatt halvány),
`s` a forrás azonosítója. A csomópontokon belül nincs beágyazás. Kérdésnél (nem URL) a híd a DuckDuckGo
HTML-találati oldalát projektálja: a találatok `link` (w 6) + `para` (kivonat) párok.

## Eszközök (TOOL_CALL nevek)

| Név | Kulcsok | Az OS-en |
|-----|---------|----------|
| `fs_read` | `path` | `open(O_READ)` + `read`, max 16 KiB |
| `fs_write` | `path`, `content` | `open(O_WRITE\|O_CREATE\|O_TRUNC)` + `write` |
| `fs_list` | `path` | `list` |
| `task_run` | `program`, `args` | `spawn` (örökölt capability-kkel) + `wait` |
| `ask_user` | `question` | konzolra írás, sor beolvasása |
| `done` | `summary` | az agent befejezi |

Minden eszköz egy-egy syscallra képződik le, tehát a `cap_check`-en megy át: a híd és a modell
nem kérhet olyat, amit az agent manifestje nem enged. Az elutasítás `error` TOOL_RESULT-ként
megy vissza a modellnek (`E_CAP: ...`), és az audit-naplóban is látszik.
