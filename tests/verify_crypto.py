#!/usr/bin/env python3
"""Prueft ap_crypto.h (SHA-1, RSA-OAEP, AES-128-CBC, PKCS#1-Signatur, Base64) gegen Python/OpenSSL."""
import base64, hashlib, os, pathlib, re, subprocess, sys
root = pathlib.Path(__file__).resolve().parent.parent
subprocess.run(["g++", "-O2", "-std=c++17", "-I" + str(root / "src"), str(root / "tests/test_apc.cpp"), "-o", "/tmp/test_apc"], check=True)
k = (root / "src/ap_key.h").read_text()
n = int(re.search(r'AP_RSA_N_HEX = "([0-9a-f]+)"', k).group(1), 16)
d = int(re.search(r'AP_RSA_D_HEX = "([0-9a-f]+)"', k).group(1), 16)
e = 65537
def mgf1(seed, l):
    o = b""; c = 0
    while len(o) < l:
        o += hashlib.sha1(seed + c.to_bytes(4, "big")).digest(); c += 1
    return o[:l]
def oaep(msg):
    db = hashlib.sha1(b"").digest() + b"\0" * (256 - len(msg) - 42) + b"\1" + msg
    seed = os.urandom(20)
    dbm = bytes(a ^ b for a, b in zip(db, mgf1(seed, len(db))))
    sm = bytes(a ^ b for a, b in zip(seed, mgf1(dbm, 20)))
    return pow(int.from_bytes(b"\0" + sm + dbm, "big"), e, n).to_bytes(256, "big")
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
key, iv, plain, data = os.urandom(16), os.urandom(16), os.urandom(48), os.urandom(32)
enc = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()
cipher = enc.update(plain) + enc.finalize()
out = subprocess.run(["/tmp/test_apc", oaep(key).hex(), key.hex(), iv.hex(), cipher.hex(), data.hex()], capture_output=True, text=True, check=True).stdout.split("\n")
res = {l.split()[0]: l.split(" ", 1)[1] for l in out if l}
em = b"\0\1" + b"\xff" * (256 - 3 - len(data)) + b"\0" + data
sig = pow(int.from_bytes(em, "big"), d, n).to_bytes(256, "big")
checks = {"SHA-1": res["sha1"] == hashlib.sha1(b"abc").hexdigest(), "RSA-OAEP": res["oaep"].split()[1] == key.hex(),
          "AES-128-CBC": res["aes"] == plain.hex(), "RSA-Signatur": res["sig"] == sig.hex(), "Base64": res["b64"] == base64.b64encode(data).decode()}
for name, ok in checks.items():
    print(f"{name:14s}", "OK" if ok else "FEHLER")
sys.exit(0 if all(checks.values()) else 1)
