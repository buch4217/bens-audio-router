// Test der adaptiven Live-Latenz (livectl.h)
#include <cmath>
#include <cstdio>
#include "livectl.h"
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FEHLER: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
int main() {
    LiveCtl::Cfg c; c.startMs = 8; LiveCtl L; L.init(c); double t = 0; uint32_t u = 0, r = 0;
    printf("1) ruhig -> Absenken bis zum Minimum\n");
    for (int i = 0; i < 100 * 600; i++) { t += 0.1; L.tick(t, u, r); }   // 10 Minuten
    printf("   nach 10 min Ruhe: %.1f ms (Minimum %.1f)\n", L.lagMs(), c.minMs); CHECK(std::fabs(L.lagMs() - c.minMs) < 1e-9, "Minimum nicht erreicht: %.2f", L.lagMs());
    printf("2) Aussetzer -> Erhoehen, aber hoechstens einmal je 'holdSec'\n");
    double before = L.lagMs(); u++; t += 0.1; L.tick(t, u, r); CHECK(L.lagMs() > before, "keine Erhoehung nach Unterlauf"); double a = L.lagMs(); u++; t += 0.1; L.tick(t, u, r); CHECK(L.lagMs() == a, "zu schnell erneut erhoeht (holdSec)");
    t += 4; u++; L.tick(t, u, r); CHECK(L.lagMs() > a, "nach holdSec keine weitere Erhoehung"); printf("   nach 3 Aussetzern: %.1f ms\n", L.lagMs());
    printf("3) Dauerprobleme: Deckel bei maxMs\n"); for (int i = 0; i < 100; i++) { t += 3.5; u++; r++; L.tick(t, u, r); } CHECK(L.lagMs() <= c.maxMs + 1e-9 && L.lagMs() >= c.maxMs - 1e-9, "Deckel: %.2f", L.lagMs());
    printf("4) wieder ruhig: sinkt langsam, bleibt aber ueber der Aussetzer-Marke (floor)\n");
    for (int i = 0; i < 10 * 60 * 10; i++) { t += 0.1; L.tick(t, u, r); } printf("   nach 10 min Ruhe: %.1f ms\n", L.lagMs()); CHECK(L.lagMs() < c.maxMs, "sinkt nicht"); 
    printf("5) manuell: Automatik aus -> Wert bleibt fest\n"); L.setAuto(false); L.setManual(12.0); for (int i = 0; i < 1000; i++) { t += 0.1; u++; L.tick(t, u, r); } CHECK(L.lagMs() == 12.0, "manueller Wert veraendert: %.2f", L.lagMs());
    L.setManual(500); CHECK(L.lagMs() == c.maxMs, "Obergrenze bei manuell"); L.setManual(0); CHECK(L.lagMs() == c.minMs, "Untergrenze bei manuell");
    printf(fails ? "\n%d FEHLER\n" : "\nalles bestanden\n", fails); return fails ? 1 : 0;
}
