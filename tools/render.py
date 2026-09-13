#!/usr/bin/env python3
"""Hu kep a hidon: Chromium (Playwright) rendereli az oldalt, a kep RGB pixelekkent, zlib-bel
tomoritve megy a netbooknak, a linkek helyeivel egyutt (docs/AOP.md, RENDER/RENDERED).
A Playwright szinkron API-ja egy szalhoz kotott, ezert egy dedikalt munkaszal szolgalja ki a keroket.
  python tools/render.py <url> [w h y]   proba: PNG-be menti a kepet (build/render.png)"""
import io
import queue
import sys
import threading
import zlib

LINKS_JS = """() => {
  const out = [];
  for (const a of document.querySelectorAll('a[href]')) {
    const r = a.getBoundingClientRect();
    if (r.width < 3 || r.height < 3) continue;
    const cs = getComputedStyle(a);
    if (cs.visibility === 'hidden' || cs.display === 'none' || parseFloat(cs.opacity) < 0.1) continue;
    const cx = r.x + r.width / 2, cy = r.y + Math.min(r.height / 2, 8);
    if (cx < 0 || cy < 0 || cx >= innerWidth || cy >= innerHeight) continue;
    const el = document.elementFromPoint(cx, cy);          // csak ami tenyleg latszik es nincs letakarva
    if (!el || !(el === a || a.contains(el) || el.contains(a))) continue;
    const href = a.href || '';
    if (!/^https?:/i.test(href)) continue;
    out.push([Math.round(r.x + scrollX), Math.round(r.y + scrollY), Math.round(r.width), Math.round(r.height), href]);
  }
  out.sort((p, q) => (p[1] - q[1]) || (p[0] - q[0]));
  return out.slice(0, 400);
}"""

UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36 AO-OS-projector"


class Renderer:
    def __init__(self):
        self.q = queue.Queue()
        self.thread = threading.Thread(target=self._worker, daemon=True)
        self.thread.start()

    def _worker(self):
        from playwright.sync_api import sync_playwright
        pw = sync_playwright().start()
        browser = pw.chromium.launch()
        page = None
        cur_url = None
        while True:
            job, reply = self.q.get()
            try:
                url, w, h, y = job
                if page is None:
                    page = browser.new_page(viewport={"width": w, "height": 768}, user_agent=UA)
                if url != cur_url:
                    if not url.lower().startswith(("http://", "https://")):
                        raise ValueError(f"nem http(s) cim: {url[:80]}")
                    cur_url = None
                    page.goto(url, wait_until="domcontentloaded", timeout=15000)
                    try:
                        page.wait_for_load_state("networkidle", timeout=3000)
                    except Exception:
                        pass
                    cur_url = url
                page_h = int(page.evaluate("Math.max(document.documentElement.scrollHeight, document.body ? document.body.scrollHeight : 0)"))
                if page_h < 768:
                    page_h = 768
                y = max(0, min(y, max(0, page_h - 768)))
                h = max(768, min(h, page_h - y))
                # a nezet maga lesz w x h, es ugyanebben az elrendezesben keszul a kep es a linklista:
                # a "teljes oldal" kep ideiglenes atmeretezese mas elrendezest adott, es a keretek elcsusztak
                if page.viewport_size != {"width": w, "height": h}:
                    page.set_viewport_size({"width": w, "height": h})
                page.evaluate(f"window.scrollTo(0, {y})")
                page.wait_for_timeout(150)
                y = int(page.evaluate("window.scrollY"))
                png = page.screenshot(type="png")
                links = page.evaluate(LINKS_JS)
                from PIL import Image
                im = Image.open(io.BytesIO(png)).convert("RGB")
                if im.size != (w, h):
                    im = im.resize((w, h))
                rgb = im.tobytes()
                title = page.title() or url
                reply.put({"w": w, "h": h, "y": y, "page_h": page_h, "title": title, "url": page.url,
                           "links": links, "rgb": rgb})
            except Exception as e:  # betoltesi/render-hiba: a hivo ERR-t kuld
                reply.put(e)

    def render(self, url, w, h, y):
        reply = queue.Queue()
        self.q.put(((url, w, h, y), reply))
        r = reply.get(timeout=120)
        if isinstance(r, Exception):
            raise r
        return r


_renderer = None


def render(url, w, h, y):
    global _renderer
    if _renderer is None:
        _renderer = Renderer()
    return _renderer.render(url, w, h, y)


def pack(r):
    """(meta szoveg, zlib-adat) a RENDERED kerethez es a FILE 'img' darabokhoz"""
    z = zlib.compress(r["rgb"], 6)
    s = sum(r["rgb"]) & 0xFFFFFFFF
    head = [f"w={r['w']}", f"h={r['h']}", f"y={r['y']}", f"page_h={r['page_h']}", f"zlen={len(z)}", f"sum={s}",
            f"title={r['title'][:180]}", f"url={r['url'][:400]}"]
    # a meta egy AOP-keret: a netbook 64 KiB-ig fogad, ezert a linklista legfeljebb ~56 KB
    links = []
    size = sum(len(l.encode("utf-8")) + 1 for l in head) + 16
    for x, y, w, h, href in r["links"]:
        line = f"{x} {y} {w} {h} {href[:300]}"
        n = len(line.encode("utf-8")) + 1
        if size + n > 56000:
            break
        links.append(line)
        size += n
    lines = head + [f"links={len(links)}"] + links
    return "\n".join(lines) + "\n", z


if __name__ == "__main__":
    url = sys.argv[1]
    w, h, y = (int(a) for a in sys.argv[2:5]) if len(sys.argv) >= 5 else (1366, 2304, 0)
    r = render(url, w, h, y)
    from PIL import Image
    Image.frombytes("RGB", (r["w"], r["h"]), r["rgb"]).save("build/render.png")
    meta, z = pack(r)
    print(meta.split("\n")[0:9], f"{len(r['rgb'])} -> {len(z)} bajt, build/render.png")
