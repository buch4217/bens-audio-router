#include "core.h"
#include <cstdio>
#include <chrono>
// misst Amplitude/Rest einer Sinus-Rekonstruktion nach Resampling
static void Test(int srcRate, int outRate, double f, double ampDbRef = 0) {
    SrcBuf s; s.init(srcRate, 8.0);
    const double PI = 3.14159265358979;
    uint64_t N = (uint64_t)(srcRate * 7.0);
    for (uint64_t i = 0; i < N; i++) { float v = (float)(0.5 * std::sin(2 * PI * f * i / srcRate)); s.push(v, v); }
    ConnState cs; cs.noCtl = true;
    int n = 4096; std::vector<float> acc;
    int total = outRate * 3; std::vector<float> out; out.reserve(total);
    // Lag so waehlen, dass Cursor bei ~1 s startet (head = 7 s)
    for (int done = 0; done < total; done += n) { acc.assign(n * 2, 0.f); ProcessConn(cs, s, outRate, 6000.0, n, acc.data(), 1.0f); for (int i = 0; i < n; i++) out.push_back(acc[2 * i]); }
    // Anfangsbereich (Fade) ueberspringen; LS-Fit einer Sinus-Komponente
    int a = outRate / 2, b = (int)out.size();
    // Phase/Amplitude mittels DFT bei f (Frequenz bezogen auf ausgegebene Samples, Quell-Zeit = Out-Zeit)
    double sc = 0, ss = 0; int cnt = b - a;
    for (int i = a; i < b; i++) { double t = (double)i / outRate; sc += out[i] * std::cos(2 * PI * f * t); ss += out[i] * std::sin(2 * PI * f * t); }
    double A = 2 * std::sqrt(sc * sc + ss * ss) / cnt;
    double res = 0, tot = 0;
    for (int i = a; i < b; i++) { double t = (double)i / outRate; double fit = (2.0 / cnt) * (sc * std::cos(2 * PI * f * t) + ss * std::sin(2 * PI * f * t)); res += (out[i] - fit) * (out[i] - fit); tot += out[i] * out[i]; }
    printf("%6d -> %5d Hz  f=%7.0f Hz  Amplitude %+7.2f dB (Ref 0.5)  Rest(THD+N) %7.1f dB\n", srcRate, outRate, f, 20 * std::log10(A / 0.5), 10 * std::log10(res / (tot + 1e-30) + 1e-30));
}
static void Alias(int srcRate, int outRate, double f) {
    SrcBuf s; s.init(srcRate, 8.0);
    const double PI = 3.14159265358979;
    uint64_t N = (uint64_t)(srcRate * 7.0);
    for (uint64_t i = 0; i < N; i++) { float v = (float)(0.5 * std::sin(2 * PI * f * i / srcRate)); s.push(v, v); }
    ConnState cs; cs.noCtl = true; int n = 4096; std::vector<float> acc; double e = 0; int cnt = 0;
    for (int done = 0; done < outRate * 2; done += n) { acc.assign(n * 2, 0.f); ProcessConn(cs, s, outRate, 6000.0, n, acc.data(), 1.0f); if (done > outRate / 2) for (int i = 0; i < n; i++) { e += acc[2 * i] * acc[2 * i]; cnt++; } }
    printf("Alias-Test %d->%d Hz: Eingangston %6.0f Hz (oberhalb Nyquist) -> Restpegel %7.1f dBFS\n", srcRate, outRate, f, 10 * std::log10(e / cnt / 0.125 + 1e-30) - 6.02);
}
int main() {
    auto t0 = std::chrono::steady_clock::now();
    GetKernel(192000, 48000); GetKernel(48000, 48000); GetKernel(192000, 44100); GetKernel(44100, 44100);
    printf("Kernel-Aufbau (4 Stueck): %.1f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    Test(192000, 48000, 1000); Test(192000, 48000, 10000); Test(192000, 48000, 19000); Test(192000, 48000, 20000);
    Test(192000, 44100, 1000); Test(192000, 44100, 19000);
    Test(48000, 48000, 1000); Test(48000, 48000, 19000); Test(44100, 44100, 18000); Test(192000, 192000, 40000);
    Alias(192000, 48000, 30000); Alias(192000, 48000, 40000); Alias(192000, 44100, 30000);
    // Performance
    SrcBuf s; s.init(192000, 8.0); for (int i = 0; i < 192000 * 7; i++) s.push(0.1f, 0.1f);
    for (int o : {48000, 192000}) { ConnState cs; cs.noCtl = true; std::vector<float> acc(o / 10 * 2); auto t1 = std::chrono::steady_clock::now(); for (int k = 0; k < 10; k++) { std::fill(acc.begin(), acc.end(), 0.f); ProcessConn(cs, s, o, 6000.0, o / 10, acc.data(), 1.0f); } double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count(); printf("CPU 192k -> %d Hz: %.1f ms pro 1 s Audio (%.1f %% eines Kerns)\n", o, ms, ms / 10.0); }
    Dither d; double mean = 0; int hist[3] = {0}; for (int i = 0; i < 100000; i++) { int16_t q = d.to16(0.0f, true); hist[q < 0 ? 0 : q == 0 ? 1 : 2]++; mean += q; } printf("TPDF-Dither bei Stille: neg %d / null %d / pos %d, Mittel %.4f LSB\n", hist[0], hist[1], hist[2], mean / 100000);
    return 0;
}
