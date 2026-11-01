// Test des Auto-Pegels (dyn.h): K-Bewertung, Lautheits-Eichung, True-Peak-Limiter, Leveler, Stille/Gate
#include <cstdio>
#include <cstdlib>
#include <random>
#include "dyn.h"
using namespace dyn;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FEHLER: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// unabhaengiger True-Peak-Pruefer: 16x Oversampling mit langem Kaiser-Sinc (brute force)
static double TruePeakRef(const std::vector<float>& x, int skip) {
    const int OS = 16, H = 64; double pk = 0; const double beta = 9.0;
    auto I0 = [](double v) { double s = 1, t = 1; for (int k = 1; k < 80; k++) { double q = v / (2.0 * k); t *= q * q; s += t; if (t < 1e-15 * s) break; } return s; };
    static std::vector<double> ker; if (ker.empty()) { ker.resize((size_t)OS * 2 * H); for (int ph = 0; ph < OS; ph++) for (int t = 0; t < 2 * H; t++) { double d = (t - H + 1) - (double)ph / OS; double sn = std::fabs(d) < 1e-12 ? 1 : std::sin(PI * d) / (PI * d); double r = d / H; double w = std::fabs(r) >= 1 ? 0 : I0(beta * std::sqrt(1 - r * r)) / I0(beta); ker[(size_t)ph * 2 * H + t] = sn * w; } }
    for (size_t i = (size_t)skip + H; i + H < x.size(); i++) for (int ph = 0; ph < OS; ph++) { double a = 0; for (int t = 0; t < 2 * H; t++) a += x[i - H + 1 + t] * ker[(size_t)ph * 2 * H + t]; pk = std::max(pk, std::fabs(a)); }
    return 20 * std::log10(std::max(pk, 1e-9));
}
static double LufsOf(const std::vector<float>& lr, int fs, double fromSec) {
    LoudMeter m; m.init(fs); for (size_t i = (size_t)(fromSec * fs); i < lr.size() / 2; i++) m.push(lr[2 * i], lr[2 * i + 1]); return m.shortTerm();
}
static std::vector<float> Sine(int fs, double f, double ampDb, double sec, double phase = 0) {
    std::vector<float> v((size_t)(sec * fs) * 2); double a = std::pow(10.0, ampDb / 20.0);
    for (size_t i = 0; i < v.size() / 2; i++) { float s = (float)(a * std::sin(2 * PI * f * i / fs + phase)); v[2 * i] = v[2 * i + 1] = s; } return v;
}
static size_t RunBlocks(AutoLevel& al, std::vector<float>& v) { size_t i = 0; for (; i + 1024 * 2 <= v.size(); i += 1024 * 2) al.process(&v[i], 1024); return i / 2; }   // verarbeitete Frames

int main() {
    printf("1) K-Bewertung 48 kHz gegen Normwerte (BS.1770)\n");
    { double b0, b1, b2, a1, a2; KShelf(48000, b0, b1, b2, a1, a2);
      CHECK(std::fabs(b0 - 1.53512485958697) < 1e-9 && std::fabs(b1 + 2.69169618940638) < 1e-9 && std::fabs(b2 - 1.19839281085285) < 1e-9 && std::fabs(a1 + 1.69065929318241) < 1e-9 && std::fabs(a2 - 0.73248077421585) < 1e-9, "Shelf-Koeffizienten weichen ab: %.12f %.12f %.12f %.12f %.12f", b0, b1, b2, a1, a2);
      KHighpass(48000, a1, a2); CHECK(std::fabs(a1 + 1.99004745483398) < 1e-9 && std::fabs(a2 - 0.99007225036621) < 1e-9, "Hochpass-Koeffizienten weichen ab: %.12f %.12f", a1, a2); }

    printf("2) Lautheits-Eichung: 997 Hz, -20 dBFS, beide Kanaele = -20,0 LUFS\n");
    for (int fs : {44100, 48000, 96000, 192000}) { auto v = Sine(fs, 997, -20, 6); double l = LufsOf(v, fs, 0); printf("   %6d Hz: %.2f LUFS\n", fs, l); CHECK(std::fabs(l + 20.0) < 0.1, "Eichung %d Hz: %.2f", fs, l); }

    printf("3) Limiter transparent unter der Decke, True-Peak-Schutz (fs/4-Sinus 45 Grad: Abtastwerte 0,707, echter Peak 1,0)\n");
    for (int fs : {48000, 96000, 192000}) {
        { AutoLevel al; al.init(fs); Params p; p.mode = 1; p.ceilingDb = -1; al.set(p); auto in = Sine(fs, 1000, -12, 1.0); auto out = in; RunBlocks(al, out);
          int L = al.latencyFrames(); double maxd = 0; for (size_t i = 4000; i + 2 * (size_t)L + 4096 < in.size() / 2; i++) maxd = std::max(maxd, (double)std::fabs(out[2 * (i + L)] - in[2 * i]));
          CHECK(maxd < 1e-6, "Limiter veraendert Signal unter der Decke (%d Hz): max. Abweichung %.2e (Latenz %d)", fs, maxd, L); }
        { AutoLevel al; al.init(fs); Params p; p.mode = 1; p.ceilingDb = -3; al.set(p); auto in = Sine(fs, fs / 4.0, 0, 2.0, PI / 4); auto out = in; size_t nf = RunBlocks(al, out);
          std::vector<float> m(nf); for (size_t i = 0; i < m.size(); i++) m[i] = out[2 * i]; double tp = TruePeakRef(m, 4096); printf("   %6d Hz: True-Peak %.2f dBTP (Decke -3,00)\n", fs, tp);
          CHECK(tp < -3.0 + 0.2, "True Peak ueber der Decke (%d Hz): %.2f dBTP", fs, tp); }
    }

    printf("4) Leveler: leise Aufnahme wird angehoben, laute abgesenkt (Ziel -18 LUFS)\n");
    for (double inDb : {-40.0, -30.0, -3.0}) {
        int fs = 48000; AutoLevel al; al.init(fs); Params p; p.mode = 2; p.targetLufs = -18; p.maxBoostDb = 30; p.ceilingDb = -1; p.speed = 2; al.set(p);
        auto v = Sine(fs, 997, inDb, 70); RunBlocks(al, v); double l = LufsOf(v, fs, 55); printf("   Eingang %6.1f dBFS -> Ausgang %.2f LUFS, Gain %.1f dB, True-Peak %.2f dBTP\n", inDb, l, al.gainDb(), al.truePeakDb());
        CHECK(std::fabs(l + 18.0) < 1.0, "Ziel verfehlt: Eingang %.0f dB -> %.2f LUFS", inDb, l);
        CHECK(al.truePeakDb() <= -1.0 + 0.2, "Decke verletzt: %.2f dBTP", al.truePeakDb());
    }
    { int fs = 48000; AutoLevel al; al.init(fs); Params p; p.mode = 2; p.targetLufs = -18; p.maxBoostDb = 12; p.speed = 2; al.set(p); auto v = Sine(fs, 997, -50, 60); RunBlocks(al, v);
      printf("   Anhebungs-Grenze: -50 dBFS, max. +12 dB -> Gain %.1f dB\n", al.gainDb()); CHECK(al.gainDb() <= 12.01f, "max. Anhebung ueberschritten: %.2f", al.gainDb()); }

    printf("5) Gate: Stille/Rauschen unter -55 LUFS wird nicht aufgepumpt, keine NaN\n");
    { int fs = 48000; AutoLevel al; al.init(fs); Params p; p.mode = 2; p.targetLufs = -18; p.maxBoostDb = 30; p.speed = 2; al.set(p);
      auto v = Sine(fs, 997, -30, 20); RunBlocks(al, v); float g0 = al.gainDb();
      std::vector<float> sil((size_t)fs * 2 * 20, 0.0f); RunBlocks(al, sil);
      std::mt19937 rg(3); std::uniform_real_distribution<float> u(-1, 1); std::vector<float> noi((size_t)fs * 2 * 20); for (auto& x : noi) x = u(rg) * 0.00003f;   // ca. -90 dBFS
      RunBlocks(al, noi); bool bad = false; for (float x : noi) if (!(x == x) || std::fabs(x) > 0.01f) bad = true;
      printf("   Gain vor Stille %.1f dB, nach Stille+Rauschen %.1f dB\n", g0, al.gainDb()); CHECK(!bad, "NaN oder Aufpumpen"); CHECK(std::fabs(al.gainDb() - g0) < 0.5f, "Gain driftet in der Stille: %.2f -> %.2f", g0, al.gainDb()); }

    printf("6) Musik-aehnliches Signal (auf 18 kHz bandbegrenztes Rauschen, Pegel-Spruenge bis +38 dB): True Peak nie ueber der Decke\n");
    auto Bursts = [&](int fs, bool limitBand) {
        std::mt19937 rg(7); std::normal_distribution<float> nd(0, 1); int N = fs * 15; std::vector<float> w((size_t)N), v((size_t)N * 2);
        for (auto& x : w) x = nd(rg);
        if (limitBand) { const int H = 128; std::vector<double> h(2 * H + 1); double fc = 18000.0 / fs; double sum = 0; auto I0 = [](double q) { double s = 1, tt = 1; for (int k = 1; k < 60; k++) { double r = q / (2.0 * k); tt *= r * r; s += tt; } return s; };
            for (int i = -H; i <= H; i++) { double sn = i == 0 ? 2 * fc : std::sin(2 * PI * fc * i) / (PI * i); double r = (double)i / H; h[i + H] = sn * I0(9.0 * std::sqrt(1 - r * r)) / I0(9.0); sum += h[i + H]; }
            std::vector<float> o((size_t)N, 0.0f); for (int i = H; i < N - H; i++) { double a2 = 0; for (int k = -H; k <= H; k++) a2 += h[k + H] * w[(size_t)(i - k)]; o[(size_t)i] = (float)(a2 / sum); } w = o; }
        double env = 0.05; for (int i = 0; i < N; i++) { if (i % (fs / 4) == 0) env = std::pow(10.0, (-40.0 + 38.0 * (rg() % 1000) / 1000.0) / 20.0); float s = (float)(env * w[(size_t)i]); v[2 * i] = s; v[2 * i + 1] = s * 0.7f; }
        for (auto& x : v) x = std::max(-3.0f, std::min(3.0f, x)); return v; };
    for (int fs : {48000, 96000, 192000}) for (int band = 1; band >= 0; band--) { auto v = Bursts(fs, band);
      AutoLevel al; al.init(fs); Params p; p.mode = 2; p.targetLufs = -16; p.ceilingDb = -1.5; p.maxBoostDb = 24; p.speed = 1; al.set(p); size_t nf = RunBlocks(al, v);
      std::vector<float> m(nf); for (size_t i = 0; i < m.size(); i++) m[i] = v[2 * i]; double tp = TruePeakRef(m, fs); double tol = band ? 0.2 : 1.3;
      printf("   %6d Hz %s: True-Peak %.2f dBTP (Decke -1,50, Toleranz %.1f dB), Sicherheits-Clamps %u\n", fs, band ? "bandbegrenzt" : "Vollband-Weissrauschen (Stresstest)", tp, tol, al.clamps());
      CHECK(tp < -1.5 + tol, "True Peak ueber der Decke (%d Hz, %s): %.2f", fs, band ? "Musik" : "Weissrauschen", tp); CHECK(al.clamps() == 0, "Sicherheits-Clamps ausgeloest: %u", al.clamps()); }
    printf(fails ? "\n%d FEHLER\n" : "\nalles bestanden\n", fails); return fails ? 1 : 0;
}
