// calib.h - Kern der akustischen Latenz-Einmessung
// Exponentieller Sinus-Sweep als Messsignal, FFT-Kreuzkorrelation (Matched Filter) gegen die
// Mikrofonaufnahme, Peak-Interpolation (parabolisch) -> Ankunftszeit mit Sub-Sample-Genauigkeit.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace calib {

inline std::vector<float> Sweep(int rate, double dur = 0.6, double f0 = 250.0, double f1 = 9000.0, double amp = 0.2) {
    const double PI = 3.14159265358979323846;
    int n = (int)(dur * rate);
    std::vector<float> s((size_t)n);
    double L = dur / std::log(f1 / f0);
    int fade = std::max(1, (int)(0.01 * rate));
    for (int i = 0; i < n; i++) {
        double t = (double)i / rate;
        double ph = 2.0 * PI * f0 * L * (std::exp(t / L) - 1.0);
        double w = 1.0;
        if (i < fade) w = 0.5 * (1.0 - std::cos(PI * i / fade));
        if (i > n - fade) w = 0.5 * (1.0 - std::cos(PI * (n - i) / fade));
        s[(size_t)i] = (float)(amp * w * std::sin(ph));
    }
    return s;
}

inline void FFT(std::vector<std::complex<float>>& a, bool inv) {
    size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * 3.14159265358979323846 / (double)len * (inv ? 1 : -1);
        std::complex<float> wl((float)std::cos(ang), (float)std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (size_t j = 0; j < len / 2; j++) {
                std::complex<float> u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
                if ((j & 255) == 255) { double a2 = ang * (double)(j + 1); w = std::complex<float>((float)std::cos(a2), (float)std::sin(a2)); }
            }
        }
    }
    if (inv) for (auto& x : a) x /= (float)n;
}

struct Peak {
    bool ok = false;
    double idx = 0;   // Startposition des Sweeps in der Aufnahme (Samples, fraktional)
    double snr = 0;   // Peak / Rauschteppich der Korrelation
};

inline Peak Locate(const std::vector<float>& mic, const std::vector<float>& probe, int rate) {
    Peak pk;
    if (mic.size() < probe.size() * 2) return pk;
    size_t N = 1;
    while (N < mic.size() + probe.size()) N <<= 1;
    std::vector<std::complex<float>> A(N), B(N);
    for (size_t i = 0; i < mic.size(); i++) A[i] = mic[i];
    for (size_t i = 0; i < probe.size(); i++) B[i] = probe[i];
    FFT(A, false);
    FFT(B, false);
    for (size_t i = 0; i < N; i++) A[i] *= std::conj(B[i]);
    FFT(A, true);
    size_t valid = mic.size() - probe.size();
    size_t bi = 0;
    float bv = 0;
    double sum2 = 0;
    for (size_t i = 0; i <= valid; i++) {
        float v = std::fabs(A[i].real());
        sum2 += (double)v * v;
        if (v > bv) { bv = v; bi = i; }
    }
    // Rauschteppich: RMS ohne +-10 ms um den Peak
    size_t ex = (size_t)(0.010 * rate);
    double s2 = 0; size_t cnt = 0;
    for (size_t i = 0; i <= valid; i++) {
        if (i + ex >= bi && i <= bi + ex) continue;
        double v = A[i].real();
        s2 += v * v;
        cnt++;
    }
    double floorRms = cnt ? std::sqrt(s2 / (double)cnt) : 1e-12;
    pk.snr = floorRms > 0 ? bv / floorRms : 0;
    double frac = 0;
    if (bi > 0 && bi < valid) {
        double y0 = std::fabs(A[bi - 1].real()), y1 = bv, y2 = std::fabs(A[bi + 1].real());
        double den = y0 - 2 * y1 + y2;
        if (std::fabs(den) > 1e-20) frac = 0.5 * (y0 - y2) / den;
    }
    pk.idx = (double)bi + frac;
    pk.ok = pk.snr > 12.0;
    (void)sum2;
    return pk;
}

}  // namespace calib
