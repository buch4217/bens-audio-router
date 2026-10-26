#!/usr/bin/env python3
"""Kodiert Test-Texte mit src/qr.h und dekodiert sie mit dem OpenCV-QR-Detektor."""
import subprocess, sys, numpy as np, cv2
texts = ["A", "https://192.168.1.5:8443/", "https://192.168.178.123:8443/#code=12345678", "https://10.0.0.7:8443/#code=00000000", "https://192.168.100.200:8443/#code=98765432&x=ä", "x" * 60, "y" * 84, "https://192.168.178.123:8443/#code=12345678 " + "z" * 60, "Z" * 104]
res = subprocess.run(["/tmp/test_qr"], input="\n".join(texts) + "\n", capture_output=True, text=True).stdout.split("\n"); i = 0; ok = True
det = cv2.QRCodeDetector()
for t in texts:
    n = int(res[i]); rows = res[i + 1:i + 1 + n]; i += 1 + n
    img = np.full((n + 8, n + 8), 255, np.uint8)
    for y, r in enumerate(rows):
        for x, ch in enumerate(r): img[y + 4, x + 4] = 0 if ch == "1" else 255
    big = cv2.resize(img, None, fx=10, fy=10, interpolation=cv2.INTER_NEAREST); dec, _, _ = det.detectAndDecode(big)
    good = dec == t; ok &= good; print(("OK     " if good else "FEHLER ") + f"Version {(n - 17) // 4}, {len(t.encode()) if False else len(t):3d} Zeichen" + ("" if good else f"  gelesen: {dec!r}"))
print("\nALLE QR-TESTS BESTANDEN" if ok else "\nQR-TESTS FEHLGESCHLAGEN"); sys.exit(0 if ok else 1)
