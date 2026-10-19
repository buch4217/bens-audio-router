#!/usr/bin/env python3
"""Prueft die vom test_wavrec erzeugten WAV-/FLAC-Dateien (Format, Bit-Genauigkeit, RF64, FLAC verlustfrei, Absturz-sicherer Header)."""
import glob, json, subprocess, sys, numpy as np
d = sys.argv[1] if len(sys.argv) > 1 else "/tmp"
RATE = 48000
def decode(p):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", p, "-f", "f32le", "-ac", "2", "-"], capture_output=True).stdout
    return np.frombuffer(raw, np.float32).reshape(-1, 2)
def probe(p):
    j = json.loads(subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-of", "json", p], capture_output=True, text=True).stdout)["streams"][0]
    return j["codec_name"], int(j["sample_rate"]), int(j["channels"])
def expect(n, off=0):
    t = (np.arange(n) + off) / RATE
    return np.stack([0.5 * np.sin(2 * np.pi * 1000 * t), 0.25 * np.sin(2 * np.pi * 440 * t)], 1)
ok = True
def check(name, cond, info=""):
    global ok; ok &= bool(cond); print(("OK     " if cond else "FEHLER ") + name + ("  " + info if info else ""))
for bits, codec, tol in ((16, "pcm_s16le", 2.5 / 32768), (24, "pcm_s24le", 1.5 / 8388608), (32, "pcm_f32le", 1e-6)):
    p = f"{d}/rec_{bits}.wav"; x = decode(p); e = expect(len(x))
    check(f"{bits} Bit: Format", probe(p) == (codec, RATE, 2), str(probe(p)))
    check(f"{bits} Bit: Laenge 3,000 s", len(x) == 3 * RATE, f"{len(x)} Frames")
    err = np.abs(x - e).max(); check(f"{bits} Bit: max. Abweichung <= {tol:.2e}", err <= tol, f"{err:.2e}")
for bits, tol in ((24, 1.5 / 8388608), (32, 1e-6)):
    p = f"{d}/rf64_{bits}.wav"; hd = open(p, "rb").read(48)
    check(f"RF64 {bits} Bit: Kennung RF64 + ds64", hd[:4] == b"RF64" and hd[12:16] == b"ds64", str(hd[:4]) + str(hd[12:16]))
    x = decode(p); e = expect(len(x))
    check(f"RF64 {bits} Bit: eine Datei, 10 s, bit-genau", len(x) == 10 * RATE and np.abs(x - e).max() <= tol, f"{len(x)} Frames, max {np.abs(x - e).max():.2e}")
check("keine Teil-Dateien (_002) angelegt", not glob.glob(f"{d}/*_002.*"))
check("kleine Aufnahme bleibt klassisches RIFF", open(f"{d}/rec_24.wav", "rb").read(4) == b"RIFF")
# FLAC: verlustfrei = identisch zur WAV-Aufnahme desselben Signals (24 Bit ohne Dither; 16 Bit mit identischer Dither-Folge)
for bits in (16, 24):
    fp = f"{d}/flac_{bits}.flac"; xf = decode(fp); xw = decode(f"{d}/rec_{bits}.wav")
    check(f"FLAC {bits} Bit: Format flac, 3,000 s (letzter Frame kuerzer)", probe(fp)[0] == "flac" and len(xf) == 3 * RATE, f"{probe(fp)} {len(xf)} Frames")
    check(f"FLAC {bits} Bit: bit-identisch zur WAV-Aufnahme (verlustfrei)", len(xf) == len(xw) and np.array_equal(xf, xw))
    ok_t = subprocess.run(["flac", "-t", "-s", fp], capture_output=True, text=True); check(f"FLAC {bits} Bit: Referenz-Decoder `flac -t` fehlerfrei", ok_t.returncode == 0, ok_t.stderr.strip()[:80])
pf = f"{d}/flac_96k.flac"; xf = decode(pf); e = expect(len(xf)) if False else None
check("FLAC 96 kHz: Rate aus STREAMINFO, 2,000 s", probe(pf)[1] == 96000 and len(decode(pf)) == 2 * 96000, str(probe(pf)))
check("FLAC 96 kHz: `flac -t` fehlerfrei", subprocess.run(["flac", "-t", "-s", pf], capture_output=True).returncode == 0)
xc = decode(f"{d}/crashf_copy.flac")
check("FLAC Absturz-Sicherheit: Kopie waehrend der Aufnahme ist lesbar", len(xc) > 0.5 * RATE, f"{len(xc) / RATE:.2f} s lesbar")
check("FLAC Absturz-Sicherheit: Inhalt korrekt", len(xc) > 0 and np.abs(xc - expect(len(xc))).max() <= 1.5 / 8388608)
x = decode(f"{d}/crash_copy.wav"); n = len(x)
check("Absturz-Sicherheit: Kopie waehrend der Aufnahme ist lesbar", n > 0.5 * RATE, f"{n / RATE:.2f} s lesbar")
check("Absturz-Sicherheit: Inhalt korrekt", n > 0 and np.abs(x - expect(n)).max() <= 1.5 / 8388608)
print("\nALLE TESTS BESTANDEN" if ok else "\nTESTS FEHLGESCHLAGEN"); sys.exit(0 if ok else 1)
