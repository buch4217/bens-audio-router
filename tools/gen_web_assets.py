#!/usr/bin/env python3
"""Bettet die Web-Oberflaeche (web/) als C-Arrays in src/web_assets.h ein (wird vom Build aufgerufen)."""
import pathlib
root = pathlib.Path(__file__).resolve().parent.parent
files = [('WEB_INDEX', 'web/index.html'), ('WEB_SW', 'web/sw.js'), ('WEB_MANIFEST', 'web/manifest.webmanifest'), ('WEB_ICON192', 'web/icon-192.png'), ('WEB_ICON512', 'web/icon-512.png'), ('WEB_ICON180', 'web/apple-touch-icon.png'), ('WEB_ICON32', 'web/icon-32.png'), ('WEB_FAVICON', 'web/favicon.ico')]
out = ['// AUTOMATISCH ERZEUGT von tools/gen_web_assets.py - nicht von Hand aendern', '#pragma once', '#include <cstddef>']
for name, rel in files:
    d = (root / rel).read_bytes()
    out.append(f'static const unsigned char {name}[] = {{' + ','.join(str(b) for b in d) + '};')
    out.append(f'static const size_t {name}_LEN = {len(d)};')
(root / 'src' / 'web_assets.h').write_text('\n'.join(out) + '\n')
print('web_assets.h:', sum((root / r).stat().st_size for _, r in files), 'Bytes eingebettet')
