// qr.h – QR-Code-Encoder (Byte-Modus, Fehlerkorrektur M, Version 1-6 = bis 106 Zeichen) (MIT, © Ben Buchholz · Buccloud Software (DE))
// Fuer den Kopplungs-Link im Web-Interface-Dialog. Portabel; mit tests/test_qr.cpp + OpenCV-Decoder geprueft.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace qr {
struct Code { int size = 0; std::vector<uint8_t> m; bool get(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && m[(size_t)y * size + x]; } };
namespace detail {
struct VerInfo { int total, ec, blocks, data; };           // ECC M: Gesamt-Codewords, EC je Block, Blockanzahl, Daten je Block
static const VerInfo VI[7] = {{0, 0, 0, 0}, {26, 10, 1, 16}, {44, 16, 1, 28}, {70, 26, 1, 44}, {100, 18, 2, 32}, {134, 24, 2, 43}, {172, 16, 4, 27}};
inline int gmul(int a, int b) { int r = 0; while (b) { if (b & 1) r ^= a; a <<= 1; if (a & 0x100) a ^= 0x11D; b >>= 1; } return r; }
inline std::vector<uint8_t> rs(const std::vector<uint8_t>& d, int ec) {
    std::vector<int> g(ec + 1, 0); g[0] = 1; int root = 1; for (int i = 0; i < ec; i++) { for (int j = ec; j >= 0; j--) { g[j] = gmul(g[j], root) ^ (j ? g[j - 1] : 0); } root = gmul(root, 2); }
    // g[0..ec]: Koeffizienten (hoechste Potenz zuletzt) -> Division
    std::vector<int> r(ec, 0); for (uint8_t b : d) { int f = b ^ r[0]; for (int i = 0; i < ec - 1; i++) r[i] = r[i + 1]; r[ec - 1] = 0; for (int i = 0; i < ec; i++) r[i] ^= gmul(g[ec - 1 - i], f); }
    return std::vector<uint8_t>(r.begin(), r.end()); }
inline bool bit(uint32_t v, int i) { return (v >> i) & 1; }
}
inline bool Encode(const std::string& text, Code& out) {
    using namespace detail; int v = 0; for (int k = 1; k <= 6; k++) if ((int)text.size() <= VI[k].data * VI[k].blocks - 2) { v = k; break; } if (!v) return false;
    const VerInfo& vi = VI[v]; int dataCw = vi.data * vi.blocks; std::vector<uint8_t> bits; auto put = [&](uint32_t val, int n) { for (int i = n - 1; i >= 0; i--) bits.push_back((val >> i) & 1); };
    put(4, 4); put((uint32_t)text.size(), 8); for (unsigned char c : text) put(c, 8); for (int i = 0; i < 4 && (int)bits.size() < dataCw * 8; i++) bits.push_back(0); while (bits.size() % 8) bits.push_back(0);
    std::vector<uint8_t> cw; for (size_t i = 0; i < bits.size(); i += 8) { int b = 0; for (int j = 0; j < 8; j++) b = (b << 1) | bits[i + j]; cw.push_back((uint8_t)b); } for (int p = 0; (int)cw.size() < dataCw; p++) cw.push_back(p % 2 ? 0x11 : 0xEC);
    std::vector<std::vector<uint8_t>> D, E; for (int b = 0; b < vi.blocks; b++) { std::vector<uint8_t> d(cw.begin() + b * vi.data, cw.begin() + (b + 1) * vi.data); D.push_back(d); E.push_back(rs(d, vi.ec)); }
    std::vector<uint8_t> fin; for (int i = 0; i < vi.data; i++) for (int b = 0; b < vi.blocks; b++) fin.push_back(D[b][i]); for (int i = 0; i < vi.ec; i++) for (int b = 0; b < vi.blocks; b++) fin.push_back(E[b][i]);
    int N = 17 + 4 * v; std::vector<uint8_t> mod((size_t)N * N, 0), fn((size_t)N * N, 0); auto setf = [&](int x, int y, bool d) { if (x < 0 || y < 0 || x >= N || y >= N) return; mod[(size_t)y * N + x] = d; fn[(size_t)y * N + x] = 1; };
    for (int i = 0; i < N; i++) { setf(6, i, i % 2 == 0); setf(i, 6, i % 2 == 0); }
    auto finder = [&](int cx, int cy) { for (int dy = -4; dy <= 4; dy++) for (int dx = -4; dx <= 4; dx++) { int dist = std::max(abs(dx), abs(dy)); setf(cx + dx, cy + dy, dist != 2 && dist != 4); } };
    finder(3, 3); finder(N - 4, 3); finder(3, N - 4);
    if (v >= 2) { int a = 4 * v + 10; int pos[2] = {6, a}; for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) { if ((i == 0 && j == 0) || (i == 0 && j == 1) || (i == 1 && j == 0)) continue; int cx = pos[i], cy = pos[j]; for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) setf(cx + dx, cy + dy, std::max(abs(dx), abs(dy)) != 1); } }
    auto fmt = [&](int mask) { uint32_t data = (0u << 3) | mask, rem = data; for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537); uint32_t b = ((data << 10) | rem) ^ 0x5412;
        for (int i = 0; i <= 5; i++) setf(8, i, bit(b, i)); setf(8, 7, bit(b, 6)); setf(8, 8, bit(b, 7)); setf(7, 8, bit(b, 8)); for (int i = 9; i < 15; i++) setf(14 - i, 8, bit(b, i));
        for (int i = 0; i < 8; i++) setf(N - 1 - i, 8, bit(b, i)); for (int i = 8; i < 15; i++) setf(8, N - 15 + i, bit(b, i)); setf(8, N - 8, true); };
    fmt(0);                                              // reserviert die Format-Felder (Inhalt wird unten ersetzt)
    size_t bi = 0; for (int right = N - 1; right >= 1; right -= 2) { if (right == 6) right = 5; for (int vert = 0; vert < N; vert++) for (int j = 0; j < 2; j++) { int x = right - j; bool up = ((right + 1) & 2) == 0; int y = up ? N - 1 - vert : vert;
            if (!fn[(size_t)y * N + x] && bi < fin.size() * 8) { mod[(size_t)y * N + x] = (fin[bi >> 3] >> (7 - (bi & 7))) & 1; bi++; } } }
    auto masked = [&](int m, int x, int y) { switch (m) { case 0: return (x + y) % 2 == 0; case 1: return y % 2 == 0; case 2: return x % 3 == 0; case 3: return (x + y) % 3 == 0; case 4: return (x / 3 + y / 2) % 2 == 0; case 5: return x * y % 2 + x * y % 3 == 0; case 6: return (x * y % 2 + x * y % 3) % 2 == 0; default: return ((x + y) % 2 + x * y % 3) % 2 == 0; } };
    int bestM = 0, bestP = 1 << 30; std::vector<uint8_t> best;
    for (int m = 0; m < 8; m++) { std::vector<uint8_t> t = mod; for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) if (!fn[(size_t)y * N + x] && masked(m, x, y)) t[(size_t)y * N + x] ^= 1;
        std::vector<uint8_t> save = mod; mod = t; fmt(m); t = mod; mod = save; int pen = 0; auto at = [&](int x, int y) { return (int)t[(size_t)y * N + x]; };
        for (int pass = 0; pass < 2; pass++) for (int a = 0; a < N; a++) { int run = 1; for (int b = 1; b < N; b++) { int c0 = pass ? at(a, b - 1) : at(b - 1, a), c1 = pass ? at(a, b) : at(b, a); if (c0 == c1) { run++; if (run == 5) pen += 3; else if (run > 5) pen++; } else run = 1; } }
        for (int y = 0; y + 1 < N; y++) for (int x = 0; x + 1 < N; x++) { int c = at(x, y); if (c == at(x + 1, y) && c == at(x, y + 1) && c == at(x + 1, y + 1)) pen += 3; }
        int dark = 0; for (uint8_t c : t) dark += c; int k = (abs(dark * 20 - N * N * 10) + N * N - 1) / (N * N) - 1; pen += std::max(0, k) * 10;
        if (pen < bestP) { bestP = pen; bestM = m; best = t; } }
    out.size = N; out.m = best; (void)bestM; return true;
}
}  // namespace qr
