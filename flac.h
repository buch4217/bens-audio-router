// flac.h - minimaler, verlustfreier FLAC-Streaming-Encoder (16/24 Bit, 2 Kanaele, Blockgroesse 4096)
// Fixed-Praediktoren Ordnung 0-4 + Rice-Kodierung (RICE2, 16 Partitionen), CONSTANT-Subframes fuer Stille,
// VERBATIM als Fallback. Streaming-tauglich: STREAMINFO mit "unbekannter Laenge" (total samples = 0).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace flac {

struct BitW {
    std::vector<uint8_t>& o;
    uint64_t acc = 0;
    int n = 0;
    explicit BitW(std::vector<uint8_t>& out) : o(out) {}
    void put(uint32_t v, int bits) {
        if (bits <= 0) return;
        uint64_t m = bits >= 32 ? 0xFFFFFFFFull : ((1ull << bits) - 1);
        acc = (acc << bits) | ((uint64_t)v & m);
        n += bits;
        while (n >= 8) { o.push_back((uint8_t)(acc >> (n - 8))); n -= 8; }
        acc &= n ? ((1ull << n) - 1) : 0;
    }
    void putS(int32_t v, int bits) { put((uint32_t)v, bits); }
    void unary(uint32_t q) {  // q Nullen, dann eine 1
        while (q >= 24) { put(0, 24); q -= 24; }
        put(1u, (int)q + 1);
    }
    void align() { if (n) put(0, 8 - n); }
};

inline uint8_t crc8(const uint8_t* d, size_t len) {
    uint8_t c = 0;
    for (size_t i = 0; i < len; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}
inline uint16_t crc16(const uint8_t* d, size_t len) {
    uint16_t c = 0;
    for (size_t i = 0; i < len; i++) {
        c ^= (uint16_t)((uint16_t)d[i] << 8);
        for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x8005) : (uint16_t)(c << 1);
    }
    return c;
}

inline void utf8num(std::vector<uint8_t>& o, uint64_t v) {
    if (v < 0x80) { o.push_back((uint8_t)v); return; }
    int nb = v < 0x800 ? 2 : v < 0x10000 ? 3 : v < 0x200000 ? 4 : v < 0x4000000 ? 5 : v < 0x80000000ull ? 6 : 7;
    static const uint8_t lead[] = {0, 0, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE};
    o.push_back((uint8_t)(lead[nb] | (uint8_t)(v >> (6 * (nb - 1)))));
    for (int i = nb - 2; i >= 0; i--) o.push_back((uint8_t)(0x80 | ((v >> (6 * i)) & 0x3F)));
}

inline std::vector<uint8_t> StreamHeader(int rate, int bps, int blk) {
    std::vector<uint8_t> h = {'f', 'L', 'a', 'C', 0x80, 0, 0, 34};
    uint8_t si[34] = {0};
    si[0] = (uint8_t)(blk >> 8); si[1] = (uint8_t)blk; si[2] = (uint8_t)(blk >> 8); si[3] = (uint8_t)blk;
    uint64_t v = ((uint64_t)rate << 44) | ((uint64_t)1 << 41) | ((uint64_t)(bps - 1) << 36);
    for (int i = 0; i < 8; i++) si[10 + i] = (uint8_t)(v >> (56 - 8 * i));
    h.insert(h.end(), si, si + 34);
    return h;
}

// Beste Rice-Parameter je Partition; liefert Bits (ohne Parameterfelder) und schreibt k[]
inline uint64_t RiceCost(const std::vector<uint32_t>& u, int N, int ord, int PO, int* kOut) {
    const int NP = 1 << PO, PS = N >> PO;
    uint64_t total = 0;
    for (int p = 0; p < NP; p++) {
        int a = p * PS + (p == 0 ? ord : 0), b = (p + 1) * PS;
        int cnt = b - a;
        uint64_t sum = 0;
        for (int i = a; i < b; i++) sum += u[i];
        int k0 = 0;
        if (cnt > 0 && sum > 0) { uint64_t mean = sum / (uint64_t)cnt; while (k0 < 30 && (1ull << (k0 + 1)) <= mean) k0++; }
        uint64_t best = ~0ull; int bk = 0;
        for (int k = std::max(0, k0 - 1); k <= std::min(30, k0 + 2); k++) {
            uint64_t bits = (uint64_t)cnt * (uint64_t)(1 + k);
            for (int i = a; i < b; i++) bits += (u[i] >> k);
            if (bits < best) { best = bits; bk = k; }
        }
        kOut[p] = bk;
        total += best;
    }
    return total;
}

inline void EncodeSub(BitW& w, const int32_t* x, int N, int bps) {
    bool same = true;
    for (int i = 1; i < N; i++) if (x[i] != x[0]) { same = false; break; }
    if (same) { w.put(0, 1); w.put(0, 6); w.put(0, 1); w.putS(x[0], bps); return; }
    const int PO = 4, NP = 1 << PO;
    int bestOrd = -1; uint64_t bestBits = ~0ull; int bestK[NP];
    std::vector<int64_t> r((size_t)N);
    std::vector<uint32_t> u((size_t)N);
    for (int ord = 0; ord <= 4; ord++) {
        for (int i = ord; i < N; i++) {
            int64_t v;
            switch (ord) {
                case 0: v = x[i]; break;
                case 1: v = (int64_t)x[i] - x[i - 1]; break;
                case 2: v = (int64_t)x[i] - 2 * (int64_t)x[i - 1] + x[i - 2]; break;
                case 3: v = (int64_t)x[i] - 3 * (int64_t)x[i - 1] + 3 * (int64_t)x[i - 2] - x[i - 3]; break;
                default: v = (int64_t)x[i] - 4 * (int64_t)x[i - 1] + 6 * (int64_t)x[i - 2] - 4 * (int64_t)x[i - 3] + x[i - 4];
            }
            r[i] = v;
            u[i] = v >= 0 ? (uint32_t)(v << 1) : (uint32_t)(((-v) << 1) - 1);
        }
        int k[NP];
        uint64_t bits = 8 + (uint64_t)ord * bps + 2 + 4 + (uint64_t)NP * 5 + RiceCost(u, N, ord, PO, k);
        if (bits < bestBits) { bestBits = bits; bestOrd = ord; memcpy(bestK, k, sizeof(k)); }
    }
    if (bestBits >= 8 + (uint64_t)N * bps) {  // VERBATIM
        w.put(0, 1); w.put(1, 6); w.put(0, 1);
        for (int i = 0; i < N; i++) w.putS(x[i], bps);
        return;
    }
    const int ord = bestOrd;
    for (int i = ord; i < N; i++) {  // Residuum der besten Ordnung neu
        int64_t v;
        switch (ord) {
            case 0: v = x[i]; break;
            case 1: v = (int64_t)x[i] - x[i - 1]; break;
            case 2: v = (int64_t)x[i] - 2 * (int64_t)x[i - 1] + x[i - 2]; break;
            case 3: v = (int64_t)x[i] - 3 * (int64_t)x[i - 1] + 3 * (int64_t)x[i - 2] - x[i - 3]; break;
            default: v = (int64_t)x[i] - 4 * (int64_t)x[i - 1] + 6 * (int64_t)x[i - 2] - 4 * (int64_t)x[i - 3] + x[i - 4];
        }
        r[i] = v;
        u[i] = v >= 0 ? (uint32_t)(v << 1) : (uint32_t)(((-v) << 1) - 1);
    }
    w.put(0, 1); w.put((uint32_t)(8 + ord), 6); w.put(0, 1);
    for (int i = 0; i < ord; i++) w.putS(x[i], bps);
    w.put(1, 2);   // RICE2 (5-Bit-Parameter)
    w.put(PO, 4);
    const int PS = N >> PO;
    for (int p = 0; p < NP; p++) {
        int a = p * PS + (p == 0 ? ord : 0), b = (p + 1) * PS, k = bestK[p];
        w.put((uint32_t)k, 5);
        for (int i = a; i < b; i++) {
            w.unary(u[i] >> k);
            if (k) w.put(u[i] & ((1u << k) - 1), k);
        }
    }
}

// Haengt einen kompletten Frame (4096 Samples/Kanal) an out an
inline void EncodeFrame(std::vector<uint8_t>& out, const int32_t* L, const int32_t* R, int N, int bps, int rate, uint64_t frameNo) {
    std::vector<uint8_t> f;
    f.reserve((size_t)N * 6 + 64);
    f.push_back(0xFF);
    f.push_back(0xF8);                     // Sync, feste Blockgroesse
    int bsCode = N == 4096 ? 12 : 7;       // 4096, sonst explizite 16-Bit-Blockgroesse (letzter, kuerzerer Frame)
    int srCode = rate == 44100 ? 9 : rate == 48000 ? 10 : 0;
    f.push_back((uint8_t)((bsCode << 4) | srCode));
    int bpsCode = bps == 16 ? 4 : bps == 24 ? 6 : 0;
    f.push_back((uint8_t)((1 << 4) | (bpsCode << 1)));  // 2 Kanaele unabhaengig
    utf8num(f, frameNo);
    if (bsCode == 7) { f.push_back((uint8_t)((N - 1) >> 8)); f.push_back((uint8_t)(N - 1)); }
    f.push_back(crc8(f.data(), f.size()));
    BitW w(f);
    EncodeSub(w, L, N, bps);
    EncodeSub(w, R, N, bps);
    w.align();
    uint16_t c = crc16(f.data(), f.size());
    f.push_back((uint8_t)(c >> 8));
    f.push_back((uint8_t)c);
    out.insert(out.end(), f.begin(), f.end());
}

}  // namespace flac
