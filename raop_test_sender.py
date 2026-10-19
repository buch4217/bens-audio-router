#!/usr/bin/env python3
"""RAOP-Testsender (AirPlay 1) fuer Bens Audio Router: Apple-Challenge, ANNOUNCE (RSA-OAEP + AES), SETUP, RECORD,
ALAC-Audio (unkomprimierte ALAC-Frames) mit Paketverlust + Resend, Lautstaerke, TEARDOWN."""
import socket, struct, os, sys, time, math, base64, hashlib, random, threading
HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"; PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
SECS = float(sys.argv[3]) if len(sys.argv) > 3 else 6.0; DROP = (sys.argv[4] == "drop") if len(sys.argv) > 4 else False
import pathlib, re
_k = (pathlib.Path(__file__).resolve().parent.parent / 'src' / 'ap_key.h').read_text()
N = int(re.search(r'AP_RSA_N_HEX = "([0-9a-f]+)"', _k).group(1), 16); E = 65537
def mgf1(seed, l):
    o = b''; c = 0
    while len(o) < l: o += hashlib.sha1(seed + c.to_bytes(4, 'big')).digest(); c += 1
    return o[:l]
def oaep_enc(msg):
    lh = hashlib.sha1(b'').digest(); db = lh + b'\0' * (256 - len(msg) - 42) + b'\1' + msg; seed = os.urandom(20)
    dbm = bytes(a ^ b for a, b in zip(db, mgf1(seed, len(db)))); sm = bytes(a ^ b for a, b in zip(seed, mgf1(dbm, 20)))
    return pow(int.from_bytes(b'\0' + sm + dbm, 'big'), E, N).to_bytes(256, 'big')
try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    def cbc(key, iv, data):
        n = len(data) // 16 * 16; e = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor(); return e.update(data[:n]) + e.finalize() + data[n:]
except ImportError:
    from Crypto.Cipher import AES
    def cbc(key, iv, data):
        n = len(data) // 16 * 16; return AES.new(key, AES.MODE_CBC, iv).encrypt(data[:n]) + data[n:]
class Bits:
    def __init__(s): s.v = 0; s.n = 0
    def put(s, val, bits): s.v = (s.v << bits) | (val & ((1 << bits) - 1)); s.n += bits
    def bytes(s):
        pad = (-s.n) % 8; v = s.v << pad; return v.to_bytes((s.n + pad) // 8, 'big')
def alac_frame(left, right):   # unkomprimierter ALAC-Frame, 16 Bit stereo
    b = Bits(); b.put(1, 3); b.put(0, 4); b.put(0, 12); b.put(1, 1); b.put(0, 2); b.put(1, 1); b.put(len(left), 32)
    for l, r in zip(left, right): b.put(l, 16); b.put(r, 16)
    b.put(7, 3); return b.bytes()
tcp = socket.create_connection((HOST, PORT), 5); cseq = [0]
def rtsp(method, hdrs="", body=b"", uri="rtsp://127.0.0.1/1234"):
    cseq[0] += 1
    req = f"{method} {uri} RTSP/1.0\r\nCSeq: {cseq[0]}\r\nUser-Agent: iTunes/10.6\r\n{hdrs}"
    if body: req += f"Content-Length: {len(body)}\r\n"
    tcp.sendall(req.encode() + b"\r\n" + body)
    data = b''
    while b"\r\n\r\n" not in data: data += tcp.recv(4096)
    head, rest = data.split(b"\r\n\r\n", 1); h = {}
    for ln in head.decode().split("\r\n")[1:]:
        if ':' in ln: k, v = ln.split(':', 1); h[k.strip().lower()] = v.strip()
    n = int(h.get('content-length', 0))
    while len(rest) < n: rest += tcp.recv(4096)
    return head.decode().split("\r\n")[0], h, rest
chal = os.urandom(16); st, h, _ = rtsp("OPTIONS", "Apple-Challenge: " + base64.b64encode(chal).decode().rstrip('=') + "\r\n", uri="*")
resp = base64.b64decode(h['apple-response'] + '==')
em = pow(int.from_bytes(resp, 'big'), E, N).to_bytes(256, 'big'); data = em[em.index(b'\0', 2) + 1:]
print("OPTIONS:", st, "| Apple-Response gueltig:", data[:16] == chal, "| Challenge+IP+MAC (32 Byte):", len(data))
key = os.urandom(16); iv = os.urandom(16)
sdp = ("v=0\r\no=iTunes 1 0 IN IP4 127.0.0.1\r\ns=iTunes\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 0 RTP/AVP 96\r\na=rtpmap:96 AppleLossless\r\n"
       "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\na=rsaaeskey:" + base64.b64encode(oaep_enc(key)).decode().rstrip('=') + "\r\na=aesiv:" + base64.b64encode(iv).decode().rstrip('=') + "\r\n")
st, h, _ = rtsp("ANNOUNCE", "Content-Type: application/sdp\r\n", sdp.encode()); print("ANNOUNCE:", st)
ctl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); ctl.bind(("0.0.0.0", 0)); tim = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); tim.bind(("0.0.0.0", 0))
st, h, _ = rtsp("SETUP", f"Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;control_port={ctl.getsockname()[1]};timing_port={tim.getsockname()[1]}\r\n")
tr = dict(kv.split('=') for kv in h['transport'].split(';') if '=' in kv); sport = int(tr['server_port']); sctl = int(tr['control_port'])
print("SETUP:", st, "| Ports:", tr)
st, h, _ = rtsp("RECORD", "Range: npt=0-\r\nRTP-Info: seq=1;rtptime=0\r\n"); print("RECORD:", st, "| Audio-Latency:", h.get('audio-latency'), "Frames =", int(h.get('audio-latency', 0)) / 44.1, "ms")
rtsp("SET_PARAMETER", "Content-Type: text/parameters\r\n", b"volume: -10.000000\r\n"); print("Lautstaerke -10 dB gesetzt")
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sent = {}; resent = [0]
def resender():
    ctl.settimeout(0.2)
    while True:
        try: d, a = ctl.recvfrom(64)
        except socket.timeout:
            if getattr(resender, 'stop', False): return
            continue
        if len(d) >= 8 and d[1] & 0x7f == 0x55:
            seq, cnt = struct.unpack('>HH', d[4:8])
            for q in range(seq, seq + cnt):
                if q in sent: ctl.sendto(b'\x80\xd6\x00\x01' + sent[q], a); resent[0] += 1
threading.Thread(target=resender, daemon=True).start()
frames = 352; total_pk = int(SECS * 44100 / frames); t0 = time.time(); ph = 0.0; ssrc = 0x1234
for i in range(total_pk):
    seq = (1 + i) & 0xffff; ts = i * frames
    left = [int(0.5 * 32767 * math.sin(2 * math.pi * 1000 * (i * frames + k) / 44100)) & 0xffff for k in range(frames)]
    frame = alac_frame(left, left); enc = cbc(key, iv, frame)
    pkt = struct.pack('>BBHII', 0x80, 0xE0 if i == 0 else 0x60, seq, ts, ssrc) + enc; sent[seq] = pkt
    if not (DROP and i % 40 == 7 and i > 10): udp.sendto(pkt, (HOST, sport))
    if i % 125 == 0: udp.sendto(struct.pack('>BBHIQI', 0x80, 0xD4, 4, max(0, ts - 11025), 0, ts), (HOST, sctl))   # Sync-Paket
    # erste 0,4 s als Burst, danach in Echtzeit
    due = t0 + max(0.0, (i * frames / 44100) - 0.4); wait = due - time.time()
    if wait > 0: time.sleep(wait)
time.sleep(0.8); resender.stop = True
print(f"Gesendet: {total_pk} Pakete ({SECS:.1f} s), absichtlich verloren: {sum(1 for i in range(total_pk) if DROP and i % 40 == 7 and i > 10)}, Resends beantwortet: {resent[0]}")
st, h, _ = rtsp("TEARDOWN"); print("TEARDOWN:", st)
