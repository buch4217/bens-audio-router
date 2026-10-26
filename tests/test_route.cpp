// Test der Kanal-Matrix des Summen-Moduls (route.h)
#include "route.h"
#include <cstdio>
static int fails = 0;
static void expect(const char* name, float l, float r, float el, float er) {
    bool ok = std::fabs(l - el) < 1e-6f && std::fabs(r - er) < 1e-6f;
    std::printf("%s %-44s -> L %.3f R %.3f (erwartet %.3f / %.3f)\n", ok ? "OK    " : "FEHLER", name, l, r, el, er);
    if (!ok) fails++;
}
static void one(const char* name, float il, float ir, int sc, int dc, float el, float er) {
    float in[2] = {il, ir}, out[2] = {0, 0}; route::Add(in, out, 1, sc, dc); expect(name, out[0], out[1], el, er);
}
int main() {
    // Eingang L=1,0 R=0,5
    one("Stereo -> Stereo (unveraendert)", 1, .5f, 0, 0, 1, .5f);
    one("nur Links -> Links", 1, .5f, 1, 1, 1, 0);
    one("nur Links -> Rechts", 1, .5f, 1, 2, 0, 1);
    one("nur Links -> Mitte", 1, .5f, 1, 3, 1, 1);
    one("nur Rechts -> Links", 1, .5f, 2, 1, .5f, 0);
    one("nur Rechts -> Rechts", 1, .5f, 2, 2, 0, .5f);
    one("nur Rechts -> Mitte", 1, .5f, 2, 3, .5f, .5f);
    one("Mono-Summe -> Links", 1, .5f, 3, 1, .75f, 0);
    one("Mono-Summe -> Rechts", 1, .5f, 3, 2, 0, .75f);
    one("Mono-Summe -> Mitte", 1, .5f, 3, 3, .75f, .75f);
    one("Mono-Summe, Ziel Stereo -> Mitte", 1, .5f, 3, 0, .75f, .75f);
    one("Stereo-Quelle -> Links (Mono-Summe)", 1, .5f, 0, 1, .75f, 0);
    one("Stereo-Quelle -> Rechts (Mono-Summe)", 1, .5f, 0, 2, 0, .75f);
    // Der Hauptfall: zwei Mono-Quellen (Mono = L = R) -> A links, B rechts
    {
        float a[2] = {0.8f, 0.8f}, b[2] = {0.25f, 0.25f}, out[2] = {0, 0};
        route::Add(a, out, 1, 3, 1); route::Add(b, out, 1, 3, 2);
        expect("Mono A -> L + Mono B -> R (Hauptfall)", out[0], out[1], 0.8f, 0.25f);
    }
    // Zwei Quellen auf dieselbe Seite werden addiert
    { float a[2] = {.3f, .3f}, b[2] = {.2f, .2f}, out[2] = {0, 0}; route::Add(a, out, 1, 3, 1); route::Add(b, out, 1, 3, 1); expect("zwei Quellen -> Links addiert", out[0], out[1], .5f, 0); }
    // Ausgangsstufe: Gain, Mono-Summe, Spitzenwert
    {
        float mix[4] = {1, 0, 0, 1}, acc[4] = {0, 0, 0, 0}, pl, pr;   // 2 Frames
        route::Finish(mix, acc, 2, .5f, false, pl, pr);
        expect("Finish Gain 0,5 Frame 1", acc[0], acc[1], .5f, 0); expect("Finish Gain 0,5 Frame 2", acc[2], acc[3], 0, .5f);
        expect("Finish Spitzenwerte", pl, pr, .5f, .5f);
        float acc2[4] = {0, 0, 0, 0}; route::Finish(mix, acc2, 2, 1.0f, true, pl, pr);
        expect("Finish Mono-Summe Frame 1", acc2[0], acc2[1], .5f, .5f);
    }
    std::printf(fails ? "\nTESTS FEHLGESCHLAGEN\n" : "\nALLE TESTS BESTANDEN\n");
    return fails ? 1 : 0;
}
