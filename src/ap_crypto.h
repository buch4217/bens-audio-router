// ap_crypto.h - kleine, abhaengigkeitsfreie Krypto-Bausteine fuer den AirPlay-(RAOP)-Empfaenger
// SHA-1, AES-128 (CBC-Entschluesselung), Base64, RSA-2048 (Montgomery) mit OAEP-Entschluesselung und PKCS#1-Signatur.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace apc {

// ---------------------------------------------------------------- SHA-1
inline void sha1(const uint8_t* d, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<uint8_t> m(d, d + len);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 7; i >= 0; i--) m.push_back((uint8_t)(bits >> (8 * i)));
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = ((uint32_t)m[off + i * 4] << 24) | ((uint32_t)m[off + i * 4 + 1] << 16) | ((uint32_t)m[off + i * 4 + 2] << 8) | m[off + i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & dd); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ dd; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & dd) | (c & dd); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ dd; k = 0xCA62C1D6u; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = dd; dd = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += dd; h[4] += e;
    }
    for (int i = 0; i < 5; i++) { out[i * 4] = (uint8_t)(h[i] >> 24); out[i * 4 + 1] = (uint8_t)(h[i] >> 16); out[i * 4 + 2] = (uint8_t)(h[i] >> 8); out[i * 4 + 3] = (uint8_t)h[i]; }
}

// ---------------------------------------------------------------- Base64
inline std::string b64enc(const uint8_t* d, size_t n, bool pad = true) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < n) v |= (uint32_t)d[i + 1] << 8;
        if (i + 2 < n) v |= d[i + 2];
        o += T[(v >> 18) & 63];
        o += T[(v >> 12) & 63];
        if (i + 1 < n) o += T[(v >> 6) & 63]; else if (pad) o += '=';
        if (i + 2 < n) o += T[v & 63]; else if (pad) o += '=';
    }
    return o;
}
inline std::vector<uint8_t> b64dec(const std::string& s) {
    std::vector<uint8_t> o;
    uint32_t v = 0;
    int bits = 0;
    for (char c : s) {
        int x;
        if (c >= 'A' && c <= 'Z') x = c - 'A';
        else if (c >= 'a' && c <= 'z') x = c - 'a' + 26;
        else if (c >= '0' && c <= '9') x = c - '0' + 52;
        else if (c == '+') x = 62;
        else if (c == '/') x = 63;
        else continue;
        v = (v << 6) | (uint32_t)x;
        bits += 6;
        if (bits >= 8) { bits -= 8; o.push_back((uint8_t)(v >> bits)); }
    }
    return o;
}

// ---------------------------------------------------------------- AES-128 (nur Entschluesselung, CBC)
struct Aes128 {
    uint8_t sb[256], isb[256];
    uint8_t rk[176];
    static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
    static uint8_t mul(uint8_t a, uint8_t b) {
        uint8_t r = 0;
        while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
        return r;
    }
    Aes128() {
        uint8_t p = 1, q = 1;
        do {
            p = p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1B : 0);
            q ^= q << 1; q ^= q << 2; q ^= q << 4;
            if (q & 0x80) q ^= 0x09;
            uint8_t x = q ^ (uint8_t)((q << 1) | (q >> 7)) ^ (uint8_t)((q << 2) | (q >> 6)) ^ (uint8_t)((q << 3) | (q >> 5)) ^ (uint8_t)((q << 4) | (q >> 4));
            sb[p] = x ^ 0x63;
        } while (p != 1);
        sb[0] = 0x63;
        for (int i = 0; i < 256; i++) isb[sb[i]] = (uint8_t)i;
    }
    void setKey(const uint8_t* key) {
        memcpy(rk, key, 16);
        uint8_t rc = 1;
        for (int i = 16; i < 176; i += 4) {
            uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
            if (i % 16 == 0) {
                uint8_t u = t[0];
                t[0] = sb[t[1]] ^ rc; t[1] = sb[t[2]]; t[2] = sb[t[3]]; t[3] = sb[u];
                rc = xt(rc);
            }
            for (int j = 0; j < 4; j++) rk[i + j] = rk[i - 16 + j] ^ t[j];
        }
    }
    void decBlock(const uint8_t* in, uint8_t* out) const {
        uint8_t s[16];
        for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[160 + i];
        for (int r = 9; r >= 0; r--) {
            uint8_t t[16];  // InvShiftRows
            for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) t[((c + rr) % 4) * 4 + rr] = s[c * 4 + rr];
            for (int i = 0; i < 16; i++) s[i] = isb[t[i]] ^ rk[r * 16 + i];
            if (r > 0) {  // InvMixColumns
                for (int c = 0; c < 4; c++) {
                    uint8_t a0 = s[c * 4], a1 = s[c * 4 + 1], a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
                    s[c * 4] = mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9);
                    s[c * 4 + 1] = mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13);
                    s[c * 4 + 2] = mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11);
                    s[c * 4 + 3] = mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14);
                }
            }
        }
        memcpy(out, s, 16);
    }
    // CBC mit fester IV; entschluesselt nur ganze 16-Byte-Bloecke, Rest bleibt unveraendert (RAOP-Verhalten)
    void cbcDecrypt(const uint8_t* iv, const uint8_t* in, uint8_t* out, size_t len) const {
        uint8_t prev[16];
        memcpy(prev, iv, 16);
        size_t full = len / 16 * 16;
        for (size_t i = 0; i < full; i += 16) {
            uint8_t c[16], p[16];
            memcpy(c, in + i, 16);
            decBlock(c, p);
            for (int j = 0; j < 16; j++) out[i + j] = p[j] ^ prev[j];
            memcpy(prev, c, 16);
        }
        if (len > full) memcpy(out + full, in + full, len - full);
    }
};

// ---------------------------------------------------------------- RSA-2048 (Montgomery, 32-Bit-Limbs)
struct Rsa2048 {
    static const int S = 64;
    uint32_t n[S], d[S], rr[S], n0inv;
    static void fromHex(const char* hex, uint32_t* w) {
        std::string h = hex;
        memset(w, 0, S * 4);
        int idx = 0;
        for (int i = (int)h.size(); i > 0 && idx < S * 8; i -= 8, idx += 8) {
            int st = i - 8 < 0 ? 0 : i - 8;
            w[idx / 8] = (uint32_t)strtoul(h.substr(st, i - st).c_str(), nullptr, 16);
        }
    }
    static int cmp(const uint32_t* a, const uint32_t* b) {
        for (int i = S - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
        return 0;
    }
    static void sub(uint32_t* a, const uint32_t* b) {
        int64_t bor = 0;
        for (int i = 0; i < S; i++) { int64_t x = (int64_t)a[i] - b[i] - bor; bor = x < 0; a[i] = (uint32_t)x; }
    }
    void init(const char* nhex, const char* dhex) {
        fromHex(nhex, n);
        fromHex(dhex, d);
        uint32_t inv = n[0];
        for (int i = 0; i < 5; i++) inv *= 2 - n[0] * inv;
        n0inv = (uint32_t)(0u - inv);
        uint32_t x[S] = {0};   // rr = 2^(2*32*S) mod n durch wiederholtes Verdoppeln
        x[0] = 1;
        for (int i = 0; i < 2 * 32 * S; i++) {
            uint32_t carry = 0;
            for (int j = 0; j < S; j++) { uint32_t nc = x[j] >> 31; x[j] = (x[j] << 1) | carry; carry = nc; }
            if (carry || cmp(x, n) >= 0) sub(x, n);
        }
        memcpy(rr, x, sizeof(x));
    }
    void mont(const uint32_t* a, const uint32_t* b, uint32_t* r) const {
        uint32_t t[S + 2] = {0};
        for (int i = 0; i < S; i++) {
            uint64_t C = 0;
            for (int j = 0; j < S; j++) { uint64_t x = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + C; t[j] = (uint32_t)x; C = x >> 32; }
            uint64_t x = (uint64_t)t[S] + C;
            t[S] = (uint32_t)x;
            t[S + 1] = (uint32_t)(x >> 32);
            uint32_t m = t[0] * n0inv;
            x = (uint64_t)t[0] + (uint64_t)m * n[0];
            C = x >> 32;
            for (int j = 1; j < S; j++) { x = (uint64_t)t[j] + (uint64_t)m * n[j] + C; t[j - 1] = (uint32_t)x; C = x >> 32; }
            x = (uint64_t)t[S] + C;
            t[S - 1] = (uint32_t)x;
            t[S] = t[S + 1] + (uint32_t)(x >> 32);
        }
        uint32_t res[S];
        memcpy(res, t, S * 4);
        if (t[S] || cmp(res, n) >= 0) sub(res, n);
        memcpy(r, res, S * 4);
    }
    // base^d mod n (Big-Endian-Bytes, 256 Byte)
    bool powD(const uint8_t* in, uint8_t* out) const {
        uint32_t b[S] = {0}, one[S] = {0}, acc[S], bm[S];
        for (int i = 0; i < 256; i++) b[(255 - i) / 4] |= (uint32_t)in[i] << (8 * ((255 - i) % 4));
        if (cmp(b, n) >= 0) return false;
        one[0] = 1;
        mont(b, rr, bm);
        mont(one, rr, acc);
        for (int bit = S * 32 - 1; bit >= 0; bit--) {
            mont(acc, acc, acc);
            if ((d[bit / 32] >> (bit % 32)) & 1) mont(acc, bm, acc);
        }
        mont(acc, one, acc);
        for (int i = 0; i < 256; i++) out[255 - i] = (uint8_t)(acc[i / 4] >> (8 * (i % 4)));
        return true;
    }
    static void mgf1(const uint8_t* seed, size_t sl, uint8_t* out, size_t ol) {
        for (uint32_t c = 0; c * 20 < ol; c++) {
            std::vector<uint8_t> b(seed, seed + sl);
            b.push_back((uint8_t)(c >> 24)); b.push_back((uint8_t)(c >> 16)); b.push_back((uint8_t)(c >> 8)); b.push_back((uint8_t)c);
            uint8_t h[20];
            sha1(b.data(), b.size(), h);
            for (size_t i = 0; i < 20 && c * 20 + i < ol; i++) out[c * 20 + i] = h[i];
        }
    }
    // RSAES-OAEP (SHA-1, leeres Label) -> Klartext
    bool oaepDecrypt(const uint8_t* ct, size_t ctLen, std::vector<uint8_t>& pt) const {
        if (ctLen != 256) return false;
        uint8_t em[256];
        if (!powD(ct, em) || em[0] != 0) return false;
        uint8_t* seed = em + 1;
        uint8_t* db = em + 21;
        uint8_t sm[20], dm[235];
        mgf1(db, 235, sm, 20);
        for (int i = 0; i < 20; i++) seed[i] ^= sm[i];
        mgf1(seed, 20, dm, 235);
        for (int i = 0; i < 235; i++) db[i] ^= dm[i];
        uint8_t lh[20];
        sha1(nullptr, 0, lh);
        if (memcmp(db, lh, 20) != 0) return false;
        int i = 20;
        while (i < 235 && db[i] == 0) i++;
        if (i >= 235 || db[i] != 1) return false;
        pt.assign(db + i + 1, db + 235);
        return true;
    }
    // PKCS#1 v1.5 Typ 1 (Signatur ueber rohe Daten, wie bei der Apple-Challenge)
    bool signRaw(const uint8_t* data, size_t len, uint8_t out[256]) const {
        if (len > 245) return false;
        uint8_t em[256];
        em[0] = 0; em[1] = 1;
        size_t ps = 256 - 3 - len;
        memset(em + 2, 0xFF, ps);
        em[2 + ps] = 0;
        memcpy(em + 3 + ps, data, len);
        return powD(em, out);
    }
};

}  // namespace apc
