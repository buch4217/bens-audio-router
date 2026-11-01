#!/usr/bin/env python3
"""Prueft die von test_wavrec_hires erzeugten Dateien bit-genau (eigener WAV-Parser, keine Wandlung durch Bibliotheken)."""
import glob, os, struct, subprocess, sys, numpy as np
d = sys.argv[1] if len(sys.argv) > 1 else "/tmp"
ok = True
def check(name, cond, info=""):
    global ok; ok &= bool(cond); print(("OK     " if cond else "FEHLER ") + name + ("  " + info if info else ""))
def pat(frames, ch, start=0):   # gleiches Muster wie im C++-Test (uint64-Arithmetik)
    k = (np.arange(start, start + frames, dtype=np.uint64))[:, None]; c = np.arange(ch, dtype=np.uint64)[None, :]
    with np.errstate(over="ignore"): v = (((k * np.uint64(2654435761) + c * np.uint64(40503 * 977)) >> np.uint64(3)) % np.uint64(16777216)).astype(np.int64)
    return (v - 8388608).astype(np.int32)
def parse(p):
    b = open(p, "rb").read(); riff = b[:4]; chunks = {}; pos = 12; ds = None
    if riff == b"RF64": ds = struct.unpack("<QQQ", b[20:44])
    while pos + 8 <= len(b):
        cid = b[pos:pos + 4]; sz = struct.unpack("<I", b[pos + 4:pos + 8])[0]
        if cid == b"data" and sz == 0xFFFFFFFF and ds: sz = ds[1]
        chunks[cid] = (pos + 8, sz)
        if cid == b"data": break
        pos += 8 + sz + (sz & 1)
    fo, fs = chunks[b"fmt "]; tag, ch, rate, br, ba, bits = struct.unpack("<HHIIHH", b[fo:fo + 16]); mask = None; sub = tag
    if tag == 0xFFFE: mask = struct.unpack("<I", b[fo + 20:fo + 24])[0]; sub = struct.unpack("<H", b[fo + 24:fo + 26])[0]
    do, dsz = chunks[b"data"]; raw = b[do:do + dsz]
    bext = None
    if b"bext" in chunks:
        bo, _ = chunks[b"bext"]; bext = dict(desc=b[bo:bo + 256].split(b"\0")[0].decode(), orig=b[bo + 256:bo + 288].split(b"\0")[0].decode(), date=b[bo + 320:bo + 330].decode(), tref=struct.unpack("<Q", b[bo + 338:bo + 346])[0])
    return dict(riff=riff, ch=ch, rate=rate, bits=bits, mask=mask, sub=sub, raw=raw, ba=ba, bext=bext, nb=len(b), chunks=chunks)
def ints(w, bits):
    if bits == 24: a = np.frombuffer(w["raw"], np.uint8).reshape(-1, 3).astype(np.int32); v = a[:, 0] | (a[:, 1] << 8) | (a[:, 2] << 16); v = np.where(v & 0x800000, v - 0x1000000, v); return v.reshape(-1, w["ch"])
    if bits == 32: return np.frombuffer(w["raw"], np.int32).reshape(-1, w["ch"])
    return np.frombuffer(w["raw"], np.int16).reshape(-1, w["ch"]).astype(np.int32)

w = parse(f"{d}/hr_16ch_96k_24.wav"); x = ints(w, 24)
check("16 Kanaele / 96 kHz / 24 Bit: Format", (w["ch"], w["rate"], w["bits"], w["sub"]) == (16, 96000, 24, 1), str((w["ch"], w["rate"], w["bits"], w["sub"])))
check("16 Kanaele: genau 5,000 s", len(x) == 96000 * 5, f"{len(x)} Frames")
check("16 Kanaele: BIT-GENAU (jedes Sample jedes Kanals identisch)", np.array_equal(x, pat(len(x), 16)), "")
check("BWF-Chunk: Beschreibung, Urheber, Zeitreferenz", w["bext"] and w["bext"]["desc"] == "16-Kanal-Test" and w["bext"]["orig"] == "BensAudioRouter" and w["bext"]["tref"] == 123456789, str(w["bext"]))
check("Blockgroesse stimmt (16 x 3 Bytes)", w["ba"] == 48)
w = parse(f"{d}/hr_int32_192k.wav"); x = ints(w, 32)
exp = (pat(len(x), 2).astype(np.int64) << 8).astype(np.int32)
check("32-Bit-Ganzzahl 192 kHz: Format (PCM, nicht Float)", (w["ch"], w["rate"], w["bits"], w["sub"]) == (2, 192000, 32, 1), str((w["ch"], w["rate"], w["bits"], w["sub"])))
check("32-Bit-Ganzzahl 192 kHz: BIT-GENAU (24-Bit-Quelle << 8)", len(x) == 192000 * 3 and np.array_equal(x, exp))
w = parse(f"{d}/hr_8ch_f32_96k.wav"); f = np.frombuffer(w["raw"], np.float32).reshape(-1, 8)
check("8 Kanaele Float: Format + Kanalmaske 7.1 (0x63F)", (w["ch"], w["bits"], w["sub"], w["mask"]) == (8, 32, 3, 0x63F), str((w["ch"], w["bits"], w["sub"], w["mask"])))
check("8 Kanaele Float: bit-identisch", np.array_equal(f, pat(len(f), 8).astype(np.float32) / 8388608.0))
w = parse(f"{d}/hr_16bit_nodither.wav"); x = ints(w, 16); e = ((np.arange(44100)[:, None] * 37 + np.array([0, 1001])[None, :]) % 65536 - 32768)
check("16 Bit ohne Dither: bit-genau", len(x) == 44100 and np.array_equal(x, e))
# Teilung
parts = sorted(glob.glob(f"{d}/hr_split*.wav")); base = [p for p in parts if os.path.basename(p) == "hr_split.wav" or os.path.basename(p).startswith("hr_split_0")]
exp_len = [48000, 48000, 48000, 24000]; got = [len(ints(parse(p), 24)) for p in base]
check("Teilung nach Dauer: 4 Dateien (1 s, 1 s, 1 s, 0,5 s)", got == exp_len, str(got) + " " + str([os.path.basename(p) for p in base]))
cat = np.concatenate([ints(parse(p), 24) for p in base]); check("Teilung: lueckenlos und bit-genau (Dateien aneinandergehaengt = Original)", np.array_equal(cat, pat(len(cat), 2)))
tr = [parse(p)["bext"]["tref"] for p in base]; check("Teilung: BWF-Zeitreferenz je Datei fortlaufend", tr == [1000, 49000, 97000, 145000], str(tr))
sb = sorted(glob.glob(f"{d}/hr_split_bytes*.wav")); sizes = [len(parse(p)["raw"]) for p in sb]
check("Teilung nach Groesse: Dateien <= 300000 Nutzbytes, lueckenlos", len(sb) >= 3 and all(s <= 300000 for s in sizes) and np.array_equal(np.concatenate([ints(parse(p), 24) for p in sb]), pat(sum(s // 6 for s in sizes), 2)), str(sizes))
fl = sorted(glob.glob(f"{d}/hr_split*.flac")); n_each = []
for p in fl:
    r = subprocess.run(["ffmpeg", "-v", "error", "-i", p, "-f", "s32le", "-"], capture_output=True).stdout; n_each.append(len(np.frombuffer(r, np.int32)) // 2)
    check(f"FLAC-Teil {os.path.basename(p)}: Referenz-Decoder fehlerfrei", subprocess.run(["flac", "-t", "-s", p], capture_output=True).returncode == 0)
check("FLAC-Teilung: 3 Dateien (1 s, 1 s, Rest), zusammen = Original", len(fl) == 3 and n_each == [48000, 48000, 10000], str(n_each))
allf = np.concatenate([(np.frombuffer(subprocess.run(["ffmpeg", "-v", "error", "-i", p, "-f", "s32le", "-"], capture_output=True).stdout, np.int32).reshape(-1, 2) >> 8) for p in fl]); check("FLAC-Teilung: bit-genau", np.array_equal(allf, pat(len(allf), 2)))
# Vorlauf
w = parse(f"{d}/hr_preroll.wav"); x = ints(w, 24)
check("Vorlauf: 3 s Datei (2 s davor + 1 s danach)", len(x) == 48000 * 3, f"{len(x)}")
check("Vorlauf: beginnt bei Frame 48000 (die letzten 2 s VOR dem Start), lueckenlos", np.array_equal(x, pat(len(x), 2, 48000)))
w = parse(f"{d}/hr_preroll_mismatch.wav"); check("Vorlauf mit falscher Kanalzahl wird verworfen (nur neue Daten)", len(ints(w, 24)) == 4800)
print("\nALLE TESTS BESTANDEN" if ok else "\nTESTS FEHLGESCHLAGEN"); sys.exit(0 if ok else 1)
