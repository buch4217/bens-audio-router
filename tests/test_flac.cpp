#include "flac.h"
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    int bps = argc > 1 ? atoi(argv[1]) : 24, rate = argc > 2 ? atoi(argv[2]) : 48000;
    const int N = 4096, FR = 120;
    std::vector<int32_t> L, R;
    uint32_t s = 1234567;
    auto rnd = [&]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (double)(int32_t)s / 2147483648.0; };
    int64_t maxv = (1ll << (bps - 1)) - 1;
    for (int i = 0; i < N * FR; i++) {
        double t = (double)i / rate; double v;
        if (i > N * 20 && i < N * 30) v = 0;                                   // digitale Stille (CONSTANT)
        else if (i > N * 100) v = rnd() * 0.9;                                   // Vollaussteuerung Rauschen (VERBATIM-Fall)
        else v = 0.4 * std::sin(2 * 3.14159265 * 440 * t) + 0.2 * std::sin(2 * 3.14159265 * 3000 * t) + 0.001 * rnd();
        L.push_back((int32_t)std::llround(v * maxv));
        R.push_back((int32_t)std::llround((i > N * 10 ? 0.7 * v : 0.5 * v) * maxv) * (i % 7919 == 0 ? -1 : 1));
    }
    std::vector<uint8_t> out = flac::StreamHeader(rate, bps, N);
    for (int f = 0; f < FR; f++) flac::EncodeFrame(out, &L[f * N], &R[f * N], N, bps, rate, f);
    FILE* fo = fopen("/tmp/t.flac", "wb"); fwrite(out.data(), 1, out.size(), fo); fclose(fo);
    // Referenz als WAV (little endian, PCM) fuer Vergleich
    FILE* fr = fopen("/tmp/t.raw", "wb");
    for (size_t i = 0; i < L.size(); i++) { int32_t a = L[i], b = R[i]; fwrite(&a, 4, 1, fr); fwrite(&b, 4, 1, fr); }
    fclose(fr);
    size_t raw = L.size() * 2 * bps / 8;
    printf("%d Bit / %d Hz: %zu Samples/Kanal -> %zu Byte FLAC (%.1f %% der PCM-Groesse)\n", bps, rate, L.size(), out.size(), 100.0 * out.size() / raw);
    return 0;
}
