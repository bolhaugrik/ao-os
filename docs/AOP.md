# AOP v1 – AO-OS ↔ híd protokoll

A netbook nem beszél HTTPS-t és JSON-t. Egy TCP-kapcsolaton keretezett üzeneteket cserél a híddal
(`tools/bridge.py`), amely a tényleges AI-API-hoz beszél. A híd cseréje nem érinti az OS-t.

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
