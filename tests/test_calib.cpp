#include "calib.h"
#include <cstdio>
#include <chrono>
int main() {
    struct T { int rate; double delay; double noise; bool reverb; };
    T tests[] = {{48000, 0.0123, 0.0, false}, {48000, 2.4137, 0.05, true}, {44100, 7.0012, 0.15, true}, {48000, 0.2, 0.3, true}, {96000, 3.14159, 0.1, true}};
    uint32_t s = 99; auto rnd = [&]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (double)(int32_t)s / 2147483648.0; };
    for (auto& t : tests) {
        auto probe = calib::Sweep(t.rate);
        size_t N = (size_t)(12.0 * t.rate);
        std::vector<float> mic(N, 0.f);
        double d = t.delay * t.rate;               // Verzoegerung in Samples (fraktional)
        // Sweep mit fraktionaler Verzoegerung einfuegen: sinc-Interpolation mit Fensterung
        for (size_t i = 0; i < probe.size(); i++) {
            double pos = d + (double)i; size_t p0 = (size_t)std::floor(pos); double fr = pos - p0;
            // lineare Aufteilung reicht fuer den Test nicht -> analytisch: Sweep neu mit Zeitversatz berechnen
            (void)p0; (void)fr;
        }
        // analytischer, zeitverschobener Sweep (exakt fraktional)
        {
            const double PI = 3.14159265358979; double dur = 0.6, f0 = 250, f1 = 9000, L = dur / std::log(f1 / f0);
            int fade = (int)(0.01 * t.rate);
            for (size_t i = 0; i < (size_t)(dur * t.rate) + 2; i++) {
                size_t idx = (size_t)std::floor(d) + i; if (idx >= N) break;
                double tt = ((double)i - (d - std::floor(d))) / t.rate; if (tt < 0 || tt > dur) continue;
                double ph = 2 * PI * f0 * L * (std::exp(tt / L) - 1.0);
                double w = 1.0; double ii = tt * t.rate; if (ii < fade) w = 0.5 * (1 - std::cos(PI * ii / fade)); if (ii > dur * t.rate - fade) w = 0.5 * (1 - std::cos(PI * (dur * t.rate - ii) / fade));
                mic[idx] += (float)(0.2 * w * std::sin(ph));
            }
        }
        if (t.reverb) { // Raumhall: spaetere, schwaechere Reflexionen + 'Raum-Rauschen'
            std::vector<float> dry = mic; int taps[] = {900, 2100, 3700, 6100, 9000};
            for (int k = 0; k < 5; k++) for (size_t i = 0; i + taps[k] < N; i++) mic[i + taps[k]] += dry[i] * (0.5f / (k + 1)) * ((k & 1) ? -1 : 1);
        }
        for (size_t i = 0; i < N; i++) mic[i] += (float)(t.noise * 0.05 * rnd());
        auto t0 = std::chrono::steady_clock::now();
        auto pk = calib::Locate(mic, probe, t.rate);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("rate %5d  Soll %8.4f ms  Messung %8.4f ms  Fehler %+7.4f ms  SNR %6.1f  ok=%d  (%.0f ms Rechenzeit)\n", t.rate, t.delay * 1000, pk.idx / t.rate * 1000, pk.idx / t.rate * 1000 - t.delay * 1000, pk.snr, pk.ok, ms);
    }
    return 0;
}
