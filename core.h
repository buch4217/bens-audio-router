// core.h - plattformunabhaengiger DSP-Kern von Bens Audio Router 3
// * Quellen schreiben in Ringpuffer (native Rate). Jeder Ausgang liest pro Verbindung mit
//   eigenem Lesezeiger exakt "Lag" Frames hinter dem Schreibkopf -> gleicher Lag = zeitgleich.
// * Resampling/Interpolation: gefensterter Sinc (Kaiser beta 9, ca. 90 dB Sperrdaempfung,
//   flach bis ~20,5 kHz bei 48 kHz Ausgang), Koeffizienten-Tabelle mit 512 Phasen + linearer
//   Phaseninterpolation (Fehler < -120 dB). Bei Dezimierung (z.B. 192 -> 48 kHz) wird sauber
//   tiefpassgefiltert, kein Aliasing.
// * PI-Regler gleicht Taktdrift (ppm) aus; Delay-Aenderungen springen sofort (10 ms Crossfade).
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

struct SrcBuf {
    std::vector<float> d;  // interleaved stereo
    size_t cap = 0;
    int rate = 48000;
    std::atomic<uint64_t> head{0};
    void init(int r, double seconds) {
        rate = r;
        cap = (size_t)(r * seconds);
        d.assign(cap * 2, 0.0f);
        head.store(0);
    }
    inline void push(float l, float r) {
        uint64_t h = head.load(std::memory_order_relaxed);
        size_t i = (size_t)(h % cap);
        d[i * 2] = l;
        d[i * 2 + 1] = r;
        head.store(h + 1, std::memory_order_release);
    }
};

struct SincKernel {
    int half = 0, taps = 0, P = 512;
    std::vector<float> tab;  // (P+1) * taps
    static double I0(double x) {
        double s = 1, t = 1;
        for (int k = 1; k < 60; k++) {
            double q = x / (2.0 * k);
            t *= q * q;
            s += t;
            if (t < 1e-14 * s) break;
        }
        return s;
    }
    void build(double srcRate, double outRate) {
        const double H = 40.0, beta = 9.0, PI = 3.14159265358979323846;
        double scale = std::max(1.0, srcRate / outRate);
        bool dec = srcRate > outRate * 1.0001;
        half = (int)std::ceil(H * scale);
        taps = 2 * half;
        double fc = dec ? 0.464 / scale : 0.5;  // Zyklen pro Quell-Sample
        double i0b = I0(beta);
        tab.assign((size_t)(P + 1) * taps, 0.0f);
        std::vector<double> row(taps);
        for (int ph = 0; ph <= P; ph++) {
            double frac = (double)ph / P, sum = 0;
            for (int k = 0; k < taps; k++) {
                double dd = (double)(k - half + 1) - frac;
                double x = 2.0 * fc * dd;
                double sn = (std::fabs(x) < 1e-12) ? 1.0 : std::sin(PI * x) / (PI * x);
                double r = dd / half;
                double w = (std::fabs(r) >= 1.0) ? 0.0 : I0(beta * std::sqrt(1.0 - r * r)) / i0b;
                row[k] = 2.0 * fc * sn * w;
                sum += row[k];
            }
            for (int k = 0; k < taps; k++) tab[(size_t)ph * taps + k] = (float)(row[k] / sum);
        }
    }
};

inline std::shared_ptr<SincKernel> GetKernel(int src, int out) {
    static std::map<std::pair<int, int>, std::shared_ptr<SincKernel>> cache;
    auto key = std::make_pair(src, out);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    auto k = std::make_shared<SincKernel>();
    k->build((double)src, (double)out);
    cache[key] = k;
    return k;
}

inline bool ReadSinc(const SrcBuf& s, const SincKernel& k, uint64_t head, double pos, float& L, float& R) {
    int64_t i = (int64_t)std::floor(pos);
    double frac = pos - (double)i;
    int64_t start = i - k.half + 1;
    if (start < 0 || i + k.half >= (int64_t)head || (double)head - (double)start > (double)s.cap - 8.0) {
        L = R = 0.0f;
        return false;
    }
    double fp = frac * k.P;
    int ph = (int)fp;
    float w = (float)(fp - ph);
    if (ph >= k.P) { ph = k.P - 1; w = 1.0f; }
    const float* c0 = &k.tab[(size_t)ph * k.taps];
    const float* c1 = c0 + k.taps;
    const int taps = k.taps;
    size_t b = (size_t)((uint64_t)start % s.cap);
    float a0L = 0, a0R = 0, a1L = 0, a1R = 0;
    if (b + (size_t)taps <= s.cap) {
        const float* d = &s.d[b * 2];
        for (int t = 0; t < taps; t++) {
            float l = d[2 * t], r = d[2 * t + 1];
            a0L += l * c0[t]; a0R += r * c0[t];
            a1L += l * c1[t]; a1R += r * c1[t];
        }
    } else {
        for (int t = 0; t < taps; t++) {
            size_t idx = (b + (size_t)t) % s.cap;
            float l = s.d[idx * 2], r = s.d[idx * 2 + 1];
            a0L += l * c0[t]; a0R += r * c0[t];
            a1L += l * c1[t]; a1R += r * c1[t];
        }
    }
    L = a0L + w * (a1L - a0L);
    R = a0R + w * (a1R - a0R);
    return true;
}

struct ConnState {
    bool init = false;
    bool noCtl = false;     // nur fuer Tests: Regler aus
    double cursor = 0;      // Lesezeiger in Quell-Frames (absolut)
    double oldCursor = 0;
    int fadeLeft = 0, fadeTotal = 0;
    double applied = 0;     // zuletzt angewandter Ziel-Lag (Quell-Frames)
    double errF = 0;        // gefilterter Lag-Fehler (ms)
    double integ = 0;       // I-Anteil
    double corr = 0;        // aktuelle Ratio-Korrektur
    double lagMs = 0;       // gemessener Lag (ms)
    int resyncs = 0;
    std::shared_ptr<SincKernel> ker;
    int kerSrc = 0, kerOut = 0;
};

// Mischt n Ausgabe-Frames dieser Verbindung auf acc (interleaved stereo, Ausgaberate outRate).
inline void ProcessConn(ConnState& cs, const SrcBuf& src, int outRate, double desiredLagMs, int n, float* acc, float g) {
    const double sr = (double)src.rate;
    if (!cs.ker || cs.kerSrc != src.rate || cs.kerOut != outRate) {
        cs.ker = GetKernel(src.rate, outRate);
        cs.kerSrc = src.rate;
        cs.kerOut = outRate;
        cs.init = false;
    }
    const SincKernel& K = *cs.ker;
    const uint64_t head = src.head.load(std::memory_order_acquire);
    double desired = desiredLagMs * sr / 1000.0;
    desired = std::min(desired, (double)src.cap * 0.8);
    desired = std::max(desired, 0.0);
    const double dt = (double)n / (double)outRate;
    const double minLag = std::max({4.0, 0.003 * sr, (double)K.half + 8.0});
    desired = std::max(desired, minLag);

    auto jump = [&](double newCursor) {
        cs.oldCursor = cs.cursor;
        cs.fadeTotal = std::max(1, outRate / 100);
        cs.fadeLeft = cs.fadeTotal;
        cs.cursor = newCursor;
    };

    if (!cs.init) {
        cs.cursor = (double)head - desired;
        cs.applied = desired;
        cs.errF = 0;
        cs.integ = 0;
        cs.corr = 0;
        cs.fadeLeft = 0;
        cs.init = true;
    } else if (!cs.noCtl) {
        if (std::fabs(desired - cs.applied) > 0.25) {
            double delta = desired - cs.applied;
            cs.applied = desired;
            jump(cs.cursor - delta);
        }
        double lag = (double)head - cs.cursor;
        if (lag < minLag || std::fabs(lag - desired) > 0.15 * sr) {
            jump((double)head - desired);
            cs.errF = 0;
            cs.integ = 0;
            cs.resyncs++;
        }
    }

    if (!cs.noCtl) {
        double lag = (double)head - cs.cursor;
        double errMs = (lag - desired) * 1000.0 / sr;
        double a = 1.0 - std::exp(-dt / 0.3);
        cs.errF += a * (errMs - cs.errF);
        cs.integ += cs.errF * 6e-6 * dt;
        cs.integ = std::max(-0.002, std::min(0.002, cs.integ));
        cs.corr = std::max(-0.005, std::min(0.005, cs.errF * 1e-4 + cs.integ));
        cs.lagMs = desired * 1000.0 / sr + cs.errF;
    }

    const double step = sr / (double)outRate * (1.0 + cs.corr);
    for (int i = 0; i < n; i++) {
        float l, r;
        ReadSinc(src, K, head, cs.cursor, l, r);
        if (cs.fadeLeft > 0) {
            float lo, ro;
            ReadSinc(src, K, head, cs.oldCursor, lo, ro);
            float t = 1.0f - (float)cs.fadeLeft / (float)cs.fadeTotal;
            l = lo * (1.0f - t) + l * t;
            r = ro * (1.0f - t) + r * t;
            cs.fadeLeft--;
            cs.oldCursor += step;
        }
        acc[2 * i] += l * g;
        acc[2 * i + 1] += r * g;
        cs.cursor += step;
    }
}

// 16-Bit-Wandlung mit TPDF-Dither (nur am Ende der Kette, z.B. fuer den Sonos-WAV-Stream)
struct Dither {
    uint32_t s1 = 0x1234567u, s2 = 0x9E3779B9u;
    inline float rnd() {  // [-0.5, 0.5)
        s1 ^= s1 << 13; s1 ^= s1 >> 17; s1 ^= s1 << 5;
        return (float)(s1 >> 8) * (1.0f / 16777216.0f) - 0.5f;
    }
    inline int16_t to16(float x, bool dither) {
        float v = x * 32768.0f;
        if (dither) v += rnd() + rnd();
        long q = std::lround(v);
        if (q > 32767) q = 32767;
        if (q < -32768) q = -32768;
        return (int16_t)q;
    }
    inline int32_t to24(float x, bool dither) {
        float v = x * 8388608.0f;
        if (dither) v += rnd() + rnd();
        long q = std::lround(v);
        if (q > 8388607) q = 8388607;
        if (q < -8388608) q = -8388608;
        return (int32_t)q;
    }
};
