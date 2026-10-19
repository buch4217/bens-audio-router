# Erzeugt docs/Anleitung.html (gebrandet, eigenstaendig mit eingebetteten Bildern) aus docs/Anleitung.md
import base64, re, markdown, pathlib
from markdown.extensions.toc import slugify_unicode
base = pathlib.Path(__file__).resolve().parent.parent / "docs"
md = (base / "Anleitung.md").read_text(encoding="utf-8")
html = markdown.markdown(md, extensions=["tables", "fenced_code", "toc", "sane_lists"],
                         extension_configs={"toc": {"slugify": slugify_unicode, "permalink": False}})
# interne Anker robust machen (Doppel-Bindestriche etc.)
ids = re.findall(r'id="([^"]+)"', html)
norm = lambda s: re.sub(r"-+", "-", s)
idmap = {norm(i): i for i in ids}
html = re.sub(r'href="#([^"]+)"', lambda m: 'href="#%s"' % idmap.get(norm(m.group(1)), m.group(1)), html)
# Bilder einbetten
def embed(m):
    p = base / m.group(1)
    if not p.exists(): return m.group(0)
    return 'src="data:image/png;base64,%s"' % base64.b64encode(p.read_bytes()).decode()
html = re.sub(r'<p align="center"><img[^>]*logo\.png[^>]*></p>', '', html, count=1)
html = re.sub(r'src="([^"]+\.png)"', embed, html)
banner = base64.b64encode((base / "banner.png").read_bytes()).decode()
logo = base64.b64encode((base / "logo.png").read_bytes()).decode()
css = """
:root{--bg:#ffffff;--fg:#1b2230;--dim:#5c6678;--acc:#1f6fff;--card:#f3f6fb;--line:#dbe2ee;--code:#eef2f9}
@media (prefers-color-scheme: dark){:root{--bg:#0e1117;--fg:#e6e9f0;--dim:#97a1b5;--acc:#6ba0ff;--card:#171b24;--line:#2a3140;--code:#161b25}}
*{box-sizing:border-box}html{scroll-behavior:smooth}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.65 'Segoe UI',system-ui,-apple-system,Roboto,Arial,sans-serif}
header.brand{background:#0a1430;border-bottom:3px solid var(--acc)}
header.brand img{display:block;margin:0 auto;max-width:1100px;width:100%;height:auto;border:none;border-radius:0}
main{max-width:980px;margin:0 auto;padding:10px 28px 60px}
h1{font-size:2rem}h2{font-size:1.5rem;margin-top:2.4rem;padding-bottom:.35rem;border-bottom:2px solid var(--acc)}
h3{font-size:1.15rem;margin-top:1.6rem;color:var(--acc)}
a{color:var(--acc);text-decoration:none}a:hover{text-decoration:underline}
img{max-width:100%;height:auto;border-radius:10px;border:1px solid var(--line);margin:.6rem 0}
p[align=center] img{border:none}
table{border-collapse:collapse;width:100%;margin:1rem 0;font-size:.95rem;background:var(--card);border-radius:10px;overflow:hidden}
th{background:var(--acc);color:#fff;text-align:left}th,td{padding:.55rem .8rem;border-bottom:1px solid var(--line);vertical-align:top}
tr:last-child td{border-bottom:none}
code{background:var(--code);padding:.1rem .38rem;border-radius:5px;font:0.92em Consolas,'DejaVu Sans Mono',monospace}
pre{background:var(--code);padding:1rem;border-radius:10px;overflow:auto}pre code{background:none;padding:0}
blockquote{margin:1rem 0;padding:.7rem 1rem;border-left:4px solid var(--acc);background:var(--card);border-radius:0 10px 10px 0}
hr{border:none;border-top:1px solid var(--line);margin:2rem 0}
.toc{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:.4rem 1.2rem}
footer{border-top:1px solid var(--line);padding:22px 28px;text-align:center;color:var(--dim);font-size:.9rem}
footer img{height:34px;width:34px;border:none;margin:0 8px -10px 0;border-radius:8px}
@media print{header.brand{min-height:120px}a{color:inherit}}
"""
page = f"""<!doctype html><html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bens Audio Router – Anleitung · Buccloud Software (DE)</title><meta name="author" content="Ben Buchholz">
<meta name="description" content="Anleitung zu Bens Audio Router – audiophiler Node-Router mit Sonos-Streaming. Buccloud Software (DE)."><style>{css}</style></head><body>
<header class="brand"><img alt="Bens Audio Router – Buccloud Software (DE)" src="data:image/png;base64,{banner}"></header>
<main>{html}</main>
<footer><img alt="" src="data:image/png;base64,{logo}"><b>Bens Audio Router 4.1</b> · © Ben Buchholz · <a href="https://www.buccloud.de">www.buccloud.de</a> · 2026<br>Software Publisher: Buccloud Software (DE) · Open Source (MIT)</footer>
</body></html>"""
(base / "Anleitung.html").write_text(page, encoding="utf-8")
print("Anleitung.html:", len(page) // 1024, "KB")
