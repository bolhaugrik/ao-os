#!/usr/bin/env python3
"""Projector, hid-oldal: letoltes, lecsupaszitas, szemantikus lenyomat (JSON) a netbooknak.
Csak a Python stdlib. A lenyomat semaja: docs/AOP.md ("Lenyomat").
  python tools/projector.py <cim | kereses>          szoveges kiiras (proba)
  python tools/projector.py --json <cim | kereses>   a JSON, ahogy a netbook kapja"""
import gzip
import json
import re
import sys
import urllib.parse
import urllib.request
from html.parser import HTMLParser

UA = "Mozilla/5.0 (X11; Linux x86_64) AO-OS-projector/0.1"
SEARCH_URL = "https://html.duckduckgo.com/html/?q="
MAX_NODES = 400
MAX_LINKS = 80
MAX_JSON = 60000

SKIP_TAGS = {"script", "style", "noscript", "svg", "template", "iframe", "select", "textarea", "button", "canvas", "object"}
NOISE_TAGS = {"nav", "header", "footer", "aside"}
NOISE_RE = re.compile(r"(^|[\s_-])(nav|menu|footer|sidebar|comment|cookie|consent|banner|advert|breadcrumb|share|social|"
                      r"related|popup|modal|newsletter|promo|toolbar|pagination)([\s_-]|$)", re.I)
BLOCK_TAGS = {"p", "h1", "h2", "h3", "h4", "h5", "h6", "li", "blockquote", "pre", "td", "th", "dt", "dd", "figcaption",
              "div", "section", "article", "main", "tr", "table", "ul", "ol", "br", "hr", "title", "body", "html", "summary", "details"}
HEAD_W = {"h1": 8, "h2": 7, "h3": 6, "h4": 5, "h5": 5, "h6": 5}


def is_url(q):
    q = q.strip()
    if re.match(r"^https?://", q, re.I):
        return True
    return " " not in q and re.match(r"^[\w.-]+\.[a-z]{2,}(:\d+)?(/.*)?$", q, re.I) is not None


def fetch(url, timeout=25, limit=3 * 1024 * 1024):
    """(vegso url, szoveg, content-type)"""
    req = urllib.request.Request(url, headers={
        "User-Agent": UA, "Accept": "text/html,application/xhtml+xml,text/plain;q=0.8,*/*;q=0.5",
        "Accept-Encoding": "gzip", "Accept-Language": "hu,en;q=0.7"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = r.read(limit)
        if r.headers.get("Content-Encoding", "").lower() == "gzip":
            data = gzip.decompress(data)
        ctype = r.headers.get("Content-Type", "")
        final = r.geturl()
    m = re.search(r"charset=([\w-]+)", ctype)
    enc = m.group(1) if m else None
    if not enc:
        m = re.search(rb"<meta[^>]+charset=[\"']?([\w-]+)", data[:8192], re.I)
        enc = m.group(1).decode("ascii", "replace") if m else "utf-8"
    try:
        text = data.decode(enc, errors="replace")
    except LookupError:
        text = data.decode("utf-8", errors="replace")
    return final, text, ctype


class Extractor(HTMLParser):
    """Blokkokat gyujt dokumentum-sorrendben; a zajt (nav, menu, script...) kihagyja."""

    def __init__(self, base, search_mode=False):
        super().__init__(convert_charrefs=True)
        self.base = base
        self.search_mode = search_mode
        self.nodes = []
        self.title = ""
        self.meta_desc = ""
        self.stack = []             # (tag, noise?, skip?)
        self.skip = 0
        self.noise = 0
        self.text = []              # az aktualis blokk darabjai
        self.link_chars = 0
        self.ctx = []               # tipus-kontextusok: h1..h6, li, blockquote, pre, td, title, result
        self.link = None            # (href, [szovegdarabok])
        self.pending_links = []
        self.row_cells = []
        self.in_tr = False
        self.seen_links = set()
        self.nlinks = 0

    # ---- segedek
    def cur_type(self):
        for c in reversed(self.ctx):
            if c in HEAD_W or c in ("li", "blockquote", "pre", "td", "title", "result", "snippet"):
                return c
        return "p"

    def add(self, t, w, x, to=None):
        x = x.strip()
        if not x:
            return
        n = {"t": t, "w": w, "s": 1, "x": x}
        if to:
            n["to"] = to
        self.nodes.append(n)

    def flush(self):
        raw = "".join(self.text)
        self.text = []
        link_chars = self.link_chars
        self.link_chars = 0
        pend = self.pending_links
        self.pending_links = []
        ctype = self.cur_type()
        if ctype == "pre":
            txt = raw.strip("\n")
        else:
            txt = re.sub(r"[ \t\r\n]+", " ", raw).strip()
        if ctype == "title":
            if not self.title:
                self.title = txt
        elif ctype == "td":
            if txt:
                self.row_cells.append(txt)
        elif ctype in ("result", "snippet"):
            if ctype == "snippet" and txt:
                self.add("para", 4, txt)
        elif txt:
            if ctype in HEAD_W:
                if len(txt) >= 2:
                    self.add("head", HEAD_W[ctype], txt)
            elif ctype == "li":
                if len(txt) >= 2 and not (link_chars > 0.7 * len(txt) and len(txt) < 120):
                    self.add("item", 4, txt)
            elif ctype == "blockquote":
                self.add("quote", 5, txt)
            elif ctype == "pre":
                self.add("code", 5, txt[:2000])
            else:
                if len(txt) >= 25 and not (link_chars > 0.6 * len(txt) and len(txt) < 200):
                    self.add("para", 3 if len(txt) < 80 else 5 if len(txt) < 300 else 6, txt)
        for text, href, w in pend:
            self.add_link(text, href, w)

    def add_link(self, text, href, w):
        if self.nlinks >= MAX_LINKS:
            return
        text = re.sub(r"\s+", " ", text).strip()
        if len(text) < 3 or len(text) > 120:
            return
        if href.startswith(("javascript:", "mailto:", "#", "data:")):
            return
        url = urllib.parse.urljoin(self.base, href).split("#")[0]
        if url in self.seen_links or not url.startswith(("http://", "https://")):
            return
        self.seen_links.add(url)
        self.nlinks += 1
        self.add("link", w, text, url)

    # ---- HTMLParser
    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        cls = (a.get("class") or "") + " " + (a.get("id") or "") + " " + (a.get("role") or "")
        skip = tag in SKIP_TAGS
        noise = tag in NOISE_TAGS or (tag not in ("html", "body", "head", "main", "article") and bool(NOISE_RE.search(cls)))
        if self.search_mode:
            noise = False
        if tag == "meta" and (a.get("name") or "").lower() == "description":
            self.meta_desc = (a.get("content") or "").strip()
        if tag in ("br", "hr", "img", "meta", "link", "input"):
            if tag == "br" and self.cur_type() == "pre":
                self.text.append("\n")
            elif tag in ("br", "hr"):
                self.flush()
            return
        if tag in BLOCK_TAGS:
            self.flush()
        self.stack.append((tag, noise, skip))
        if skip:
            self.skip += 1
        if noise:
            self.noise += 1
        if tag in HEAD_W or tag in ("li", "blockquote", "pre", "td", "th", "title"):
            self.ctx.append("td" if tag == "th" else tag)
        if tag == "tr":
            self.in_tr = True
            self.row_cells = []
        if self.search_mode:
            if tag == "a" and "result__a" in cls:
                self.ctx.append("result")
                self.link = (a.get("href") or "", [], 6)
                return
            if "result__snippet" in cls:
                self.ctx.append("snippet")
                return
        if tag == "a" and a.get("href") and not self.skip and not self.noise and not self.search_mode:
            self.link = (a["href"], [], 2)

    def handle_endtag(self, tag):
        if tag in ("br", "hr", "img", "meta", "link", "input"):
            return
        if tag == "a" and self.link is not None:
            href, parts, w = self.link
            self.link = None
            text = "".join(parts)
            self.link_chars += len(text)
            if self.search_mode and self.ctx and self.ctx[-1] == "result":
                self.ctx.pop()
                self.text = []
                self.pending_links.append((text, self.unwrap_ddg(href), w))
                self.flush()
                return
            self.pending_links.append((text, href, w))
        if self.search_mode and self.ctx and self.ctx[-1] == "snippet" and tag in ("a", "div", "span", "td"):
            self.flush()
            self.ctx.pop()
            return
        if tag in BLOCK_TAGS:
            self.flush()
        # a verem visszatekerese eddig a tagig
        for i in range(len(self.stack) - 1, -1, -1):
            t, noise, skip = self.stack[i]
            if t == tag:
                for _t, n2, s2 in self.stack[i:]:
                    if s2:
                        self.skip -= 1
                    if n2:
                        self.noise -= 1
                del self.stack[i:]
                break
        if tag in HEAD_W or tag in ("li", "blockquote", "pre", "td", "th", "title"):
            want = "td" if tag == "th" else tag
            if want in self.ctx:
                idx = len(self.ctx) - 1 - self.ctx[::-1].index(want)
                del self.ctx[idx:]
        if tag == "tr" and self.in_tr:
            self.in_tr = False
            if self.row_cells:
                self.add("row", 3, " | ".join(self.row_cells))
            self.row_cells = []

    def handle_data(self, data):
        if self.skip or self.noise or not data:
            return
        if self.link is not None:
            self.link[1].append(data)
        self.text.append(data)

    @staticmethod
    def unwrap_ddg(href):
        """//duckduckgo.com/l/?uddg=<url>&... -> url"""
        if "uddg=" in href:
            q = urllib.parse.urlparse(href).query
            u = urllib.parse.parse_qs(q).get("uddg")
            if u:
                return u[0]
        if href.startswith("//"):
            return "https:" + href
        return href


def extract(html_text, base, search_mode=False, title=None):
    ex = Extractor(base, search_mode)
    ex.feed(html_text)
    ex.close()
    ex.flush()
    nodes = []
    t = title or ex.title or base
    nodes.append({"t": "title", "w": 9, "s": 1, "x": t[:200]})
    if ex.meta_desc and not search_mode:
        nodes.append({"t": "note", "w": 4, "s": 1, "x": ex.meta_desc[:400]})
    nodes.extend(ex.nodes)
    return {"v": 1, "q": base, "title": t[:200], "sources": [{"id": 1, "url": base, "title": t[:200]}], "nodes": nodes}


def plain_text_imprint(text, base):
    nodes = [{"t": "title", "w": 9, "s": 1, "x": base[:200]}]
    for para in re.split(r"\n\s*\n", text):
        p = para.strip()
        if p:
            nodes.append({"t": "code" if "\n" in p and len(p) < 2000 else "para", "w": 4, "s": 1, "x": p[:2000]})
    return {"v": 1, "q": base, "title": base[:200], "sources": [{"id": 1, "url": base, "title": base[:200]}], "nodes": nodes}


def project(q, depth=0, log=None):
    q = q.strip()
    if is_url(q):
        url = q if re.match(r"^https?://", q, re.I) else "http://" + q
        final, text, ctype = fetch(url)
        if log:
            log(f"letoltve: {final} ({len(text)} karakter, {ctype.split(';')[0]})")
        if "html" not in ctype.lower() and not text.lstrip()[:1] == "<":
            return plain_text_imprint(text, final)
        imp = extract(text, final)
    else:
        url = SEARCH_URL + urllib.parse.quote_plus(q)
        final, text, ctype = fetch(url)
        if log:
            log(f"kereses: {q!r} ({len(text)} karakter)")
        imp = extract(text, final, search_mode=True, title=f"Kereses: {q}")
        imp["q"] = q
    imp["nodes"] = imp["nodes"][:MAX_NODES]
    return imp


def to_json(imp, limit=MAX_JSON):
    """JSON bajtok; ha tul nagy, a legkisebb sulyu csomopontokat hagyja el a vegerol."""
    while True:
        data = json.dumps(imp, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        if len(data) <= limit or len(imp["nodes"]) <= 2:
            return data
        nodes = imp["nodes"]
        minw = min(n["w"] for n in nodes[1:])
        for i in range(len(nodes) - 1, 0, -1):
            if nodes[i]["w"] == minw:
                del nodes[i]
                break
        else:
            del nodes[-1]


def dump_text(imp):
    out = [f"# {imp['title']}"]
    for n in imp["nodes"]:
        if n["t"] == "link":
            out.append(f"-> {n['x']}  ({n.get('to', '')})")
        else:
            out.append(f"[{n['t']} {n['w']}] {n['x']}")
    return "\n".join(out)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--json"]
    if not args:
        sys.exit(__doc__)
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    imp = project(" ".join(args), log=lambda m: print(f"[{m}]", file=sys.stderr))
    if "--json" in sys.argv:
        sys.stdout.buffer.write(to_json(imp))
    else:
        print(dump_text(imp))
        print(f"({len(imp['nodes'])} csomopont, JSON {len(to_json(imp))} bajt)")
