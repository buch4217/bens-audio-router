// route.h – Kanal-Matrix des Summen-Moduls (MIT, © Ben Buchholz · Buccloud Software (DE))
// Pro Kabel in das Modul: welcher Kanal der Quelle (srcCh) geht wohin (dstCh).
//   srcCh: 0 Stereo (L+R)   1 nur Links   2 nur Rechts   3 Mono-Summe ((L+R)/2)
//   dstCh: 0 Stereo (unveraendert)   1 Links   2 Rechts   3 Mitte (beide Kanaele)
// Typischer Fall: zwei Mono-Quellen -> erste "Mono-Summe -> Links", zweite "Mono-Summe -> Rechts".
#pragma once
#include <algorithm>
#include <cmath>
namespace route {

// Addiert 'in' (interleaved Stereo) gemaess Zuordnung auf 'out'.
inline void Add(const float* in, float* out, int frames, int srcCh, int dstCh) {
    if (srcCh == 0 && dstCh == 0) { for (int i = 0; i < frames * 2; i++) out[i] += in[i]; return; }
    const int d = (srcCh != 0 && dstCh == 0) ? 3 : dstCh;   // Mono-Quelle ohne Ziel -> in die Mitte
    for (int i = 0; i < frames; i++) {
        const float l = in[2 * i], r = in[2 * i + 1];
        const float m = srcCh == 1 ? l : srcCh == 2 ? r : (l + r) * 0.5f;   // Stereo -> Mono: Summe
        if (d == 1 || d == 3) out[2 * i] += m;
        if (d == 2 || d == 3) out[2 * i + 1] += m;
    }
}

// Pegel des Moduls: Gain, optional Mono-Summe am Ausgang; addiert auf 'acc' des Ausgangs, liefert Spitzenwerte.
inline void Finish(const float* mix, float* acc, int frames, float gain, bool mono, float& pl, float& pr) {
    pl = pr = 0;
    for (int i = 0; i < frames; i++) {
        float l = mix[2 * i] * gain, r = mix[2 * i + 1] * gain;
        if (mono) l = r = (l + r) * 0.5f;
        pl = std::max(pl, std::fabs(l)); pr = std::max(pr, std::fabs(r));
        acc[2 * i] += l; acc[2 * i + 1] += r;
    }
}

}  // namespace route
