// webcore.h – Web-Interface-Kern (MIT, © Ben Buchholz · Buccloud Software (DE))
// Portabel (ohne Windows-Header): HTTP/1.1, WebSocket (RFC 6455), Kopplung per Einmal-Code, Sitzungen, Ratenbegrenzung, JSON-Hilfen.
// Transport (TLS/Socket) und Backend (Router-Zustand/Befehle) werden von aussen eingehaengt -> mit tests/test_webcore.cpp nativ testbar.
// Protokoll: docs/WEB-API.md. Der Server vertraut dem Client nie: alle Regeln prueft das Backend.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace web {

// ---------------------------------------------------------------- Zufall (Plattform setzt die Quelle: BCryptGenRandom / urandom)
inline std::function<void(uint8_t*, size_t)>& RngHook() { static std::function<void(uint8_t*, size_t)> f; return f; }
inline void Random(uint8_t* p, size_t n) { if (RngHook()) RngHook()(p, n); else { fprintf(stderr, "webcore: kein CSPRNG gesetzt\n"); abort(); } }

// ---------------------------------------------------------------- Hashes / Kodierung
inline uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
inline uint32_t ror(uint32_t v, int s) { return (v >> s) | (v << (32 - s)); }
inline std::string Sha1(const std::string& in) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0}; std::string m = in; uint64_t bits = (uint64_t)in.size() * 8; m += (char)0x80; while (m.size() % 64 != 56) m += (char)0;
    for (int i = 7; i >= 0; i--) m += (char)(bits >> (8 * i));
    for (size_t o = 0; o < m.size(); o += 64) {
        uint32_t w[80]; for (int i = 0; i < 16; i++) w[i] = ((uint8_t)m[o + 4 * i] << 24) | ((uint8_t)m[o + 4 * i + 1] << 16) | ((uint8_t)m[o + 4 * i + 2] << 8) | (uint8_t)m[o + 4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) { uint32_t f, k; if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; } else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; } else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; } else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i]; e = d; d = c; c = rol(b, 30); b = a; a = t; }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::string out; for (int i = 0; i < 5; i++) for (int j = 3; j >= 0; j--) out += (char)(h[i] >> (8 * j)); return out;
}
inline std::string Sha256(const std::string& in) {
    static const uint32_t K[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}; std::string m = in; uint64_t bits = (uint64_t)in.size() * 8; m += (char)0x80; while (m.size() % 64 != 56) m += (char)0;
    for (int i = 7; i >= 0; i--) m += (char)(bits >> (8 * i));
    for (size_t o = 0; o < m.size(); o += 64) {
        uint32_t w[64]; for (int i = 0; i < 16; i++) w[i] = ((uint8_t)m[o + 4 * i] << 24) | ((uint8_t)m[o + 4 * i + 1] << 16) | ((uint8_t)m[o + 4 * i + 2] << 8) | (uint8_t)m[o + 4 * i + 3];
        for (int i = 16; i < 64; i++) { uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10); w[i] = w[i - 16] + s0 + w[i - 7] + s1; }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) { uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K[i] + w[i], S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2; }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::string out; for (int i = 0; i < 8; i++) for (int j = 3; j >= 0; j--) out += (char)(h[i] >> (8 * j)); return out;
}
inline std::string Hex(const std::string& s) { static const char* d = "0123456789abcdef"; std::string o; for (unsigned char c : s) { o += d[c >> 4]; o += d[c & 15]; } return o; }
inline std::string B64(const std::string& s, bool url = false) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; static const char* u = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"; const char* a = url ? u : t; std::string o; size_t i = 0;
    for (; i + 2 < s.size(); i += 3) { uint32_t v = ((uint8_t)s[i] << 16) | ((uint8_t)s[i + 1] << 8) | (uint8_t)s[i + 2]; o += a[v >> 18]; o += a[(v >> 12) & 63]; o += a[(v >> 6) & 63]; o += a[v & 63]; }
    if (i + 1 == s.size()) { uint32_t v = (uint8_t)s[i] << 16; o += a[v >> 18]; o += a[(v >> 12) & 63]; if (!url) o += "=="; }
    else if (i + 2 == s.size()) { uint32_t v = ((uint8_t)s[i] << 16) | ((uint8_t)s[i + 1] << 8); o += a[v >> 18]; o += a[(v >> 12) & 63]; o += a[(v >> 6) & 63]; if (!url) o += '='; }
    return o;
}
inline bool ConstEq(const std::string& a, const std::string& b) { if (a.size() != b.size()) return false; unsigned char d = 0; for (size_t i = 0; i < a.size(); i++) d |= (unsigned char)(a[i] ^ b[i]); return d == 0; }
inline int64_t NowSec() { return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
inline int64_t NowMs() { return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---------------------------------------------------------------- JSON (flaches Objekt lesen, Strings schreiben)
struct JVal { enum T { NUL, BOOL, NUM, STR } t = NUL; bool b = false; double n = 0; std::string s; };
using JObj = std::map<std::string, JVal>;
inline void Utf8(std::string& o, uint32_t c) { if (c < 0x80) o += (char)c; else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 63)); } else { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); } }
inline bool ParseFlatJson(const std::string& s, JObj& out) {   // nur {"k":Wert,...} ohne Verschachtelung, max. 32 Felder
    size_t i = 0, n = s.size(); auto ws = [&] { while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++; };
    auto str = [&](std::string& r) -> bool { if (i >= n || s[i] != '"') return false; i++; r.clear();
        while (i < n && s[i] != '"') { unsigned char c = s[i++]; if (c < 0x20) return false;
            if (c == '\\') { if (i >= n) return false; char e = s[i++]; switch (e) { case '"': r += '"'; break; case '\\': r += '\\'; break; case '/': r += '/'; break; case 'b': r += '\b'; break; case 'f': r += '\f'; break; case 'n': r += '\n'; break; case 'r': r += '\r'; break; case 't': r += '\t'; break;
                case 'u': { if (i + 4 > n) return false; uint32_t cp = 0; for (int k = 0; k < 4; k++) { char h = s[i++]; cp <<= 4; if (h >= '0' && h <= '9') cp |= h - '0'; else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10; else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10; else return false; } Utf8(r, cp); break; } default: return false; } }
            else r += (char)c; if (r.size() > 2048) return false; }
        if (i >= n) return false; i++; return true; };
    out.clear(); ws(); if (i >= n || s[i] != '{') return false; i++; ws(); if (i < n && s[i] == '}') return true;
    for (;;) { ws(); std::string k; if (!str(k)) return false; ws(); if (i >= n || s[i] != ':') return false; i++; ws(); JVal v;
        if (i < n && s[i] == '"') { v.t = JVal::STR; if (!str(v.s)) return false; }
        else if (s.compare(i, 4, "true") == 0) { v.t = JVal::BOOL; v.b = true; i += 4; } else if (s.compare(i, 5, "false") == 0) { v.t = JVal::BOOL; v.b = false; i += 5; } else if (s.compare(i, 4, "null") == 0) { i += 4; }
        else { size_t st = i; while (i < n && (isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E')) i++; if (i == st) return false; char* e = nullptr; std::string num = s.substr(st, i - st); v.n = strtod(num.c_str(), &e); if (!e || *e) return false; v.t = JVal::NUM; }
        if (out.size() >= 32) return false; out[k] = v; ws(); if (i < n && s[i] == ',') { i++; continue; } if (i < n && s[i] == '}') { i++; ws(); return i == n; } return false; }
}
inline std::string JStr(const std::string& s) { std::string o = "\""; for (unsigned char c : s) { if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\"; else if (c < 0x20) { char b[8]; snprintf(b, 8, "\\u%04x", c); o += b; } else o += (char)c; } return o + "\""; }
inline bool GetNum(const JObj& o, const char* k, double& v) { auto i = o.find(k); if (i == o.end() || i->second.t != JVal::NUM || !(i->second.n == i->second.n)) return false; v = i->second.n; return true; }
inline bool GetBool(const JObj& o, const char* k, bool& v) { auto i = o.find(k); if (i == o.end() || i->second.t != JVal::BOOL) return false; v = i->second.b; return true; }
inline bool GetStr(const JObj& o, const char* k, std::string& v) { auto i = o.find(k); if (i == o.end() || i->second.t != JVal::STR) return false; v = i->second.s; return true; }

// ---------------------------------------------------------------- Schnittstellen
struct Transport {
    virtual ~Transport() {}
    virtual int Read(void* p, int n) = 0;                 // >0 Bytes, 0 geschlossen, <0 Fehler/Timeout
    virtual bool WriteAll(const void* p, int n) = 0;
    virtual void SetTimeout(int ms) = 0;                  // 0 = unbegrenzt
    virtual void Close() = 0;
    virtual std::string PeerIp() = 0;
};
struct Backend {
    virtual ~Backend() {}
    virtual std::string StateFields() = 0;                // innere Felder: "running":..,"autopilot":..,"nodes":[..],"conns":[..],"scenes":[..]
    virtual std::string MeterJson() = 0;                  // {"t":"m","a":[..]} oder ""
    virtual bool Exec(const JObj& cmd, std::string& err) = 0;   // fuehrt den Befehl im richtigen Thread aus und prueft ALLE Regeln
    virtual void OnBinary(const std::string& deviceId, const std::string& data) { (void)deviceId; (void)data; }   // Binaer-Frames (z. B. Handy-Mikrofon)
};
struct Asset { std::string mime, data; bool cache = false; };

// ---------------------------------------------------------------- Server
class Server {
public:
    std::string scheme = "https";                        // fuer die Origin-Pruefung ("http" nur in Tests)
    std::string version = "3.0";
    int maxWs = 8, cmdPerSec = 60, pairTtlSec = 120, failLimit = 5, lockSec = 60;
    Server(Backend* b, const std::string& stateDir) : be_(b), dir_(stateDir) { Load(); }
    ~Server() { Stop(); }
    void SetIndex(const std::string& html) { index_ = html; auto grab = [&](const char* a, const char* z) { size_t s = html.find(a); if (s == std::string::npos) return std::string(); s += strlen(a); size_t e = html.rfind(z); return html.substr(s, e - s); };
        scriptHash_ = "'sha256-" + B64(Sha256(grab("<script>", "</script>"))) + "'"; styleHash_ = "'sha256-" + B64(Sha256(grab("<style>", "</style>"))) + "'"; }
    void AddAsset(const std::string& path, const std::string& mime, const std::string& data, bool cache = false) { std::lock_guard<std::mutex> l(m_); assets_[path] = Asset{mime, data, cache}; }
    void SetCaPem(const std::string& pem) { std::lock_guard<std::mutex> l(m_); ca_ = pem; }
    std::string NewPairCode() { uint8_t b[4]; std::string c; for (;;) { Random(b, 4); uint32_t v = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]; if (v < 4000000000u) { char t[16]; snprintf(t, 16, "%08u", v % 100000000u); c = t; break; } } std::lock_guard<std::mutex> l(m_); code_ = c; codeExp_ = NowMs() + (int64_t)pairTtlSec * 1000; return c; }
    void SetPairCode(const std::string& c) { std::lock_guard<std::mutex> l(m_); code_ = c; codeExp_ = NowMs() + (int64_t)pairTtlSec * 1000; }     // nur Tests
    struct Dev { std::string id, name; int64_t exp = 0; };
    std::vector<Dev> Devices() { std::lock_guard<std::mutex> l(m_); std::vector<Dev> v; for (auto& d : dev_) v.push_back({d.id, d.name, d.exp}); return v; }
    bool Revoke(const std::string& id) { bool ok = false; { std::lock_guard<std::mutex> l(m_); for (size_t i = 0; i < dev_.size(); i++) if (dev_[i].id == id) { dev_.erase(dev_.begin() + i); ok = true; break; } if (ok) Save(); }
        if (ok) { std::lock_guard<std::mutex> l(wm_); for (auto* c : ws_) if (c->dev == id) c->t->Close(); } return ok; }
    int WsCount() { std::lock_guard<std::mutex> l(wm_); return (int)ws_.size(); }
    void Start() { if (run_) return; run_ = true; tick_ = std::thread([this] { Ticker(); }); }
    void Stop() { if (!run_) return; run_ = false; cv_.notify_all(); if (tick_.joinable()) tick_.join(); std::vector<Transport*> ts; { std::lock_guard<std::mutex> l(wm_); for (auto* c : ws_) ts.push_back(c->t.get()); } for (auto* t : ts) t->Close();
        for (int i = 0; i < 100 && WsCount() > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    void Handle(std::unique_ptr<Transport> t);           // eine Verbindung (blockierend, im eigenen Thread aufrufen)

private:
    struct Sess { std::string hash, id, name; int64_t exp = 0; };
    struct Ws { std::unique_ptr<Transport> t; std::mutex sm; std::string dev; std::vector<int64_t> win; };
    struct Req { std::string method, path, body; std::map<std::string, std::string> h; };
    Backend* be_; std::string dir_, index_, scriptHash_, styleHash_, ca_, code_, lastState_;
    int64_t codeExp_ = 0; std::vector<Sess> dev_; std::map<std::string, Asset> assets_; std::map<std::string, std::pair<int, int64_t>> fails_;
    std::mutex m_, wm_, lm_; std::set<Ws*> ws_; std::atomic<bool> run_{false}; std::thread tick_; std::condition_variable cv_; std::mutex cvm_;

    static std::string FileRead(const std::string& p) { std::string r;
#ifdef _WIN32
        int n = MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, nullptr, 0); std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, &w[0], n); FILE* f = _wfopen(w.c_str(), L"rb");
#else
        FILE* f = fopen(p.c_str(), "rb");
#endif
        if (!f) return r; char b[4096]; size_t k; while ((k = fread(b, 1, sizeof b, f)) > 0) r.append(b, k); fclose(f); return r; }
    static bool FileWrite(const std::string& p, const std::string& d) {
#ifdef _WIN32
        int n = MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, nullptr, 0); std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, &w[0], n); FILE* f = _wfopen(w.c_str(), L"wb");
#else
        FILE* f = fopen(p.c_str(), "wb");
#endif
        if (!f) return false; bool ok = fwrite(d.data(), 1, d.size(), f) == d.size(); fclose(f); return ok; }
    std::string DevFile() const { return dir_ + "/web_devices.txt"; }
    void Load() { std::string s = FileRead(DevFile()); size_t p = 0; dev_.clear(); while (p < s.size()) { size_t e = s.find('\n', p); if (e == std::string::npos) e = s.size(); std::string l = s.substr(p, e - p); p = e + 1;
            std::vector<std::string> f; size_t q = 0; for (;;) { size_t t = l.find('\t', q); if (t == std::string::npos) { f.push_back(l.substr(q)); break; } f.push_back(l.substr(q, t - q)); q = t + 1; }
            if (f.size() == 4 && f[3].size() == 64) { Sess d; d.id = f[0]; d.name = f[1]; d.exp = atoll(f[2].c_str()); d.hash = f[3]; if (d.exp > NowSec()) dev_.push_back(d); } } }
    void Save() { std::string s; for (auto& d : dev_) s += d.id + "\t" + d.name + "\t" + std::to_string(d.exp) + "\t" + d.hash + "\n"; FileWrite(DevFile(), s); }
    bool Auth(const Req& r, Sess& out) { auto it = r.h.find("cookie"); if (it == r.h.end()) return false; size_t p = it->second.find("bar_sid="); if (p == std::string::npos) return false; p += 8; size_t e = p; while (e < it->second.size() && (isalnum((unsigned char)it->second[e]) || it->second[e] == '-' || it->second[e] == '_')) e++;
        std::string tok = it->second.substr(p, e - p); if (tok.size() != 43) return false; std::string h = Hex(Sha256(tok)); std::lock_guard<std::mutex> l(m_); for (auto& d : dev_) if (d.exp > NowSec() && ConstEq(d.hash, h)) { out = d; return true; } return false; }
    std::string Hdr(const Req& r, const std::string& mime, size_t len, const std::string& extra = "", bool cache = false) {
        std::string host = r.h.count("host") ? r.h.at("host") : "localhost"; for (char& c : host) if (!(isalnum((unsigned char)c) || c == '.' || c == ':' || c == '-' || c == '[' || c == ']')) c = '_';
        return "Content-Type: " + mime + "\r\nContent-Length: " + std::to_string(len) + "\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nX-Frame-Options: DENY\r\nCross-Origin-Opener-Policy: same-origin\r\nCache-Control: " + (cache ? "max-age=86400" : "no-store") +
            "\r\nContent-Security-Policy: default-src 'none'; script-src " + scriptHash_ + "; style-src " + styleHash_ + "; img-src 'self' data:; connect-src 'self' wss://" + host + "; manifest-src 'self'; worker-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n" + extra; }
    void Respond(Transport& t, const Req& r, int code, const std::string& mime, const std::string& body, const std::string& extra = "", bool cache = false) {
        const char* st = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 429 ? "Too Many Requests" : code == 413 ? "Payload Too Large" : "Error";
        std::string h = "HTTP/1.1 " + std::to_string(code) + " " + st + "\r\n" + Hdr(r, mime, body.size(), extra, cache) + "\r\n"; t.WriteAll(h.data(), (int)h.size()); if (r.method != "HEAD" && !body.empty()) t.WriteAll(body.data(), (int)body.size()); }
    void Json(Transport& t, const Req& r, int code, const std::string& body, const std::string& extra = "") { Respond(t, r, code, "application/json", body, extra); }
    bool ReadReq(Transport& t, Req& r) {
        std::string buf; char b[2048]; size_t he = std::string::npos;
        while ((he = buf.find("\r\n\r\n")) == std::string::npos) { if (buf.size() > 16384) return false; int n = t.Read(b, sizeof b); if (n <= 0) return false; buf.append(b, n); }
        std::string head = buf.substr(0, he); buf = buf.substr(he + 4); size_t le = head.find("\r\n"); std::string line = head.substr(0, le); size_t s1 = line.find(' '), s2 = line.rfind(' '); if (s1 == std::string::npos || s2 <= s1) return false;
        r.method = line.substr(0, s1); r.path = line.substr(s1 + 1, s2 - s1 - 1); size_t q = r.path.find('?'); if (q != std::string::npos) r.path.resize(q);
        size_t p = le == std::string::npos ? head.size() : le + 2; while (p < head.size()) { size_t e = head.find("\r\n", p); if (e == std::string::npos) e = head.size(); std::string l = head.substr(p, e - p); p = e + 2; size_t c = l.find(':'); if (c == std::string::npos) continue;
            std::string k = l.substr(0, c), v = l.substr(c + 1); for (char& ch : k) ch = (char)tolower((unsigned char)ch); while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1); r.h[k] = v; }
        if (r.h.count("transfer-encoding")) return false; size_t cl = r.h.count("content-length") ? (size_t)atoll(r.h["content-length"].c_str()) : 0; if (cl > 4096) { Respond(t, r, 413, "application/json", "{\"err\":\"zu gross\"}"); return false; }
        while (buf.size() < cl) { int n = t.Read(b, sizeof b); if (n <= 0) return false; buf.append(b, n); } r.body = buf.substr(0, cl); return true; }
    bool OriginOk(const Req& r) { auto o = r.h.find("origin"); if (o == r.h.end()) return true; auto h = r.h.find("host"); return h != r.h.end() && o->second == scheme + "://" + h->second; }
    static std::string Cookie(const std::string& tok, long age) { return "Set-Cookie: bar_sid=" + tok + "; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=" + std::to_string(age) + "\r\n"; }
    std::string StateJson(const std::string& devId) { std::string d = "["; { std::lock_guard<std::mutex> l(m_); for (size_t i = 0; i < dev_.size(); i++) d += std::string(i ? "," : "") + "{\"id\":" + JStr(dev_[i].id) + ",\"name\":" + JStr(dev_[i].name) + ",\"me\":" + (dev_[i].id == devId ? "true" : "false") + "}"; } d += "]";
        return "{\"t\":\"state\",\"ver\":" + JStr(version) + "," + be_->StateFields() + ",\"devices\":" + d + "}"; }
    static void SendFrame(Ws& w, int op, const std::string& p) { std::string f; f += (char)(0x80 | op); size_t n = p.size(); if (n < 126) f += (char)n; else { f += (char)126; f += (char)(n >> 8); f += (char)(n & 255); } f += p; std::lock_guard<std::mutex> l(w.sm); w.t->WriteAll(f.data(), (int)f.size()); }
    void Broadcast(const std::string& msg, bool state) { std::vector<Ws*> v; { std::lock_guard<std::mutex> l(wm_); v.assign(ws_.begin(), ws_.end()); } for (auto* w : v) SendFrame(*w, 1, state ? StateJson(w->dev) : msg); }
    void Ticker() { int n = 0; while (run_) { { std::unique_lock<std::mutex> l(cvm_); cv_.wait_for(l, std::chrono::milliseconds(50)); } if (!run_) break; if (!WsCount()) { n = 0; continue; }
            std::string m = be_->MeterJson(); if (!m.empty()) Broadcast(m, false);
            if (++n % 5 == 0) { std::string s = be_->StateFields(); bool ch; { std::lock_guard<std::mutex> l(lm_); ch = s != lastState_; if (ch) lastState_ = s; } if (ch) Broadcast("", true); }
            if (n % 400 == 0) { std::vector<Ws*> v; { std::lock_guard<std::mutex> l(wm_); v.assign(ws_.begin(), ws_.end()); } for (auto* w : v) SendFrame(*w, 9, ""); } } }
    void WsLoop(Ws& w, const Req& r);
};

inline void Server::WsLoop(Ws& w, const Req& r) {
    (void)r; std::string buf; char b[2048]; w.t->SetTimeout(0);
    SendFrame(w, 1, StateJson(w.dev)); { std::string st0 = be_->StateFields(); std::lock_guard<std::mutex> l(lm_); lastState_ = st0; }
    for (;;) {
        for (;;) {
            if (buf.size() < 2) break; unsigned char b0 = buf[0], b1 = buf[1]; int op = b0 & 15; bool fin = b0 & 0x80, mask = b1 & 0x80; size_t n = b1 & 127, o = 2;
            if (!mask || !fin || (op > 2 && op < 8) || op == 0 || op > 10) return;                              // maskiert Pflicht, keine Fragmente
            if (n == 126) { if (buf.size() < 4) break; n = ((unsigned char)buf[2] << 8) | (unsigned char)buf[3]; o = 4; } else if (n == 127) return;
            if (n > (op == 2 ? 16384u : 4096u)) return; if (buf.size() < o + 4 + n) break; std::string p = buf.substr(o + 4, n); for (size_t i = 0; i < n; i++) p[i] ^= buf[o + (i & 3)]; buf.erase(0, o + 4 + n);
            if (op == 2) { be_->OnBinary(w.dev, p); continue; } if (op == 8) { SendFrame(w, 8, ""); return; } if (op == 9) { SendFrame(w, 10, p); continue; } if (op == 10) continue;
            JObj j; std::string t; double id = 0; if (!ParseFlatJson(p, j) || !GetStr(j, "t", t) || t != "cmd" || !GetNum(j, "id", id) || id != (int64_t)id) continue;
            int64_t now = NowMs(); w.win.erase(std::remove_if(w.win.begin(), w.win.end(), [&](int64_t x) { return now - x > 1000; }), w.win.end());
            std::string ack = "{\"t\":\"ack\",\"id\":" + std::to_string((int64_t)id) + ",\"ok\":";
            if ((int)w.win.size() >= cmdPerSec) { SendFrame(w, 1, ack + "false,\"err\":\"zu schnell\"}"); continue; } w.win.push_back(now);
            std::string op2, err; GetStr(j, "op", op2); bool ok;
            if (op2 == "revoke") { std::string d; ok = GetStr(j, "device", d) && Revoke(d); if (!ok) err = "Geraet fehlt"; if (ok && d == w.dev) return; }
            else if (op2 == "ping") ok = true; else ok = be_->Exec(j, err);
            SendFrame(w, 1, ack + (ok ? "true" : "false") + ",\"err\":" + JStr(ok ? "" : err) + "}"); if (ok) { { std::string st1 = be_->StateFields(); std::lock_guard<std::mutex> l(lm_); lastState_ = st1; } Broadcast("", true); }
        }
        int n = w.t->Read(b, sizeof b); if (n <= 0) return; buf.append(b, n); if (buf.size() > 70000) return;
    }
}

inline void Server::Handle(std::unique_ptr<Transport> tp) {
    Transport& t = *tp; t.SetTimeout(10000); Req r; if (!ReadReq(t, r)) { t.Close(); return; } std::string ip = t.PeerIp(); Sess s; bool authed = false;
    if (r.method == "GET" || r.method == "HEAD") {
        if (r.path == "/ws") {
            authed = Auth(r, s); auto up = r.h.find("upgrade"); auto key = r.h.find("sec-websocket-key");
            if (!authed || !OriginOk(r) || up == r.h.end() || key == r.h.end() || key->second.size() > 64 || WsCount() >= maxWs) { Respond(t, r, 401, "application/json", "{\"err\":\"nicht erlaubt\"}"); t.Close(); return; }
            std::string acc = B64(Sha1(key->second + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")); std::string h = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + acc + "\r\n\r\n"; t.WriteAll(h.data(), (int)h.size());
            Ws w; w.dev = s.id; w.t = std::move(tp); { std::lock_guard<std::mutex> l(wm_); ws_.insert(&w); } WsLoop(w, r); { std::lock_guard<std::mutex> l(wm_); ws_.erase(&w); } w.t->Close(); return;
        }
        if (r.path == "/") Respond(t, r, 200, "text/html; charset=utf-8", index_);
        else if (r.path == "/api/state") { if (!Auth(r, s)) Json(t, r, 401, "{\"err\":\"nicht gekoppelt\"}"); else Json(t, r, 200, StateJson(s.id)); }
        else if (r.path == "/ca.crt") { std::string pem; { std::lock_guard<std::mutex> l(m_); pem = ca_; } if (pem.empty()) Json(t, r, 404, "{\"err\":\"keine CA\"}"); else Respond(t, r, 200, "application/x-x509-ca-cert", pem, "Content-Disposition: attachment; filename=\"bens-audio-router-ca.crt\"\r\n"); }
        else { Asset a; bool f = false; { std::lock_guard<std::mutex> l(m_); auto i = assets_.find(r.path); if (i != assets_.end()) { a = i->second; f = true; } } if (f) Respond(t, r, 200, a.mime, a.data, "", a.cache); else Json(t, r, 404, "{\"err\":\"nicht gefunden\"}"); }
    } else if (r.method == "POST") {
        auto x = r.h.find("x-bar"); if (x == r.h.end() || x->second != "1" || !OriginOk(r)) { Json(t, r, 403, "{\"err\":\"verboten\"}"); t.Close(); return; }
        if (r.path == "/api/pair") {
            { std::lock_guard<std::mutex> l(m_); auto f = fails_.find(ip); if (f != fails_.end() && f->second.first >= failLimit && NowSec() < f->second.second) { Json(t, r, 429, "{\"err\":\"zu viele Versuche\"}", "Retry-After: " + std::to_string(lockSec) + "\r\n"); t.Close(); return; } }
            JObj j; std::string code, name; bool ok = ParseFlatJson(r.body, j) && GetStr(j, "code", code); GetStr(j, "name", name);
            { std::lock_guard<std::mutex> l(m_); ok = ok && !code_.empty() && NowMs() <= codeExp_ && ConstEq(code, code_);
                if (!ok) { auto& f = fails_[ip]; f.first++; if (f.first >= failLimit) f.second = NowSec() + lockSec; } else { code_.clear(); fails_.erase(ip); } }
            if (!ok) { Json(t, r, 401, "{\"err\":\"Code falsch oder abgelaufen\"}"); t.Close(); return; }
            uint8_t rb[38]; Random(rb, 38); std::string tok = B64(std::string((char*)rb, 32), true), id = Hex(std::string((char*)rb + 32, 6)), nm; for (char c : name) { if (nm.size() >= 40) break; if (c != '<' && c != '>' && c != '"' && c != '&' && c != '\\' && (unsigned char)c >= 0x20 && c != '\t') nm += c; } if (nm.empty()) nm = "Geraet";
            { std::lock_guard<std::mutex> l(m_); Sess d; d.hash = Hex(Sha256(tok)); d.id = id; d.name = nm; d.exp = NowSec() + 30 * 86400; dev_.push_back(d); if (dev_.size() > 32) dev_.erase(dev_.begin()); Save(); }
            Json(t, r, 200, "{\"ok\":true}", Cookie(tok, 30 * 86400));
        } else if (r.path == "/api/logout") {
            if (Auth(r, s)) Revoke(s.id); Json(t, r, 200, "{\"ok\":true}", "Set-Cookie: bar_sid=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0\r\n");
        } else Json(t, r, 404, "{\"err\":\"nicht gefunden\"}");
    } else Json(t, r, 400, "{\"err\":\"Methode\"}");
    t.Close();
}

}  // namespace web
