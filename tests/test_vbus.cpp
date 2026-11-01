// Test der virtuellen Bruecke (vbus.h): Ringe, Ueberlauf/Unterlauf, Parallelbetrieb, Surround-Zuordnung, Downmix
#include <cstdio>
#include <thread>
#include "vbus.h"
using namespace vbus;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FEHLER: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static float Val(uint64_t f, int c) { return (float)(((f * 16 + (uint64_t)c) % 1000003ull)) / 1000003.0f; }   // eindeutiger Wert je Frame/Kanal
struct Mem { std::vector<uint8_t> raw; Header* h; Mem(uint32_t rf) : raw(TotalBytes(rf) + 64, 0) { h = reinterpret_cast<Header*>((reinterpret_cast<uintptr_t>(raw.data()) + 63) & ~(uintptr_t)63); InitHeader(h, rf); } };
static void Fill(Header* h, uint64_t from, size_t n) { std::vector<float> b(n * CH); for (size_t i = 0; i < n; i++) for (int c = 0; c < CH; c++) b[i * CH + c] = Val(from + i, c); Write(h->outW, OutRing(h), h->ringFrames, b.data(), n); }

int main() {
    printf("1) Kopf und Groessen\n");
    { Mem m(4096); CHECK(Valid(m.h), "Kopf ungueltig"); CHECK(sizeof(Header) <= HEADER_BYTES, "Kopf zu gross"); CHECK(TotalBytes(4096) == 256 + 2 * 4096 * 16 * 4, "Gesamtgroesse"); m.h->magic = 0; CHECK(!Valid(m.h), "falsche Kennung wird akzeptiert"); }

    printf("2) Schreiben/Lesen mit Ringumbruch, kein Frame verloren oder vertauscht\n");
    { Mem m(4096); Reader r; r.sync = true; r.pos = 0; std::vector<float> out(512 * CH); uint64_t next = 0, read = 0; bool bad = false;
      for (int round = 0; round < 200; round++) { size_t n = 100 + (round * 37) % 400; Fill(m.h, next, n); next += n;
          size_t got; while ((got = ReadAll(r, m.h->outW, OutRing(m.h), m.h->ringFrames, out.data(), 512, 0)) > 0) { for (size_t i = 0; i < got; i++) for (int c = 0; c < CH; c++) if (out[i * CH + c] != Val(read + i, c)) bad = true; read += got; } }
      CHECK(!bad && read == next && r.drops == 0, "Daten fehlerhaft/verloren: gelesen %llu von %llu, Drops %u", (unsigned long long)read, (unsigned long long)next, r.drops); printf("   %llu Frames ueber den Ringumbruch, alles identisch\n", (unsigned long long)read); }

    printf("3) Leser zu langsam: Ueberlauf wird erkannt, Leser setzt neu auf (kein Absturz, keine alten Daten)\n");
    { Mem m(4096); Reader r; std::vector<float> out(256 * CH); Fill(m.h, 0, 100); ReadAll(r, m.h->outW, OutRing(m.h), 4096, out.data(), 256, 0);
      Fill(m.h, 100, 3900); size_t got = ReadAll(r, m.h->outW, OutRing(m.h), 4096, out.data(), 256, 64);
      CHECK(r.drops == 1, "Ueberlauf nicht erkannt (drops=%u)", r.drops); CHECK(got > 0 && out[0] == Val(4000 - 64, 0), "nach dem Neuaufsetzen falscher Startframe"); }

    printf("4) Treiber-Seite (IN-Ring): Vorlauf, Unterlauf = Stille, Aufholen ohne Dauerverzoegerung\n");
    { Mem m(8192); Reader r; std::vector<float> dst(128 * CH); size_t n = 128, target = 256;
      auto push = [&](uint64_t from, size_t k) { std::vector<float> b(k * CH); for (size_t i = 0; i < k; i++) for (int c = 0; c < CH; c++) b[i * CH + c] = Val(from + i, c); Write(m.h->inW, InRing(m.h), m.h->ringFrames, b.data(), k); };
      size_t got = ReadExact(r, m.h->inW, InRing(m.h), 8192, dst.data(), n, target, 2048); CHECK(got == 0 && dst[0] == 0.0f, "ohne Daten muss Stille kommen");
      push(0, 500); got = ReadExact(r, m.h->inW, InRing(m.h), 8192, dst.data(), n, target, 2048); CHECK(got == n, "mit genug Vorlauf muss gelesen werden"); CHECK(dst[0] == Val(500 - target, 0), "Start muss genau 'target' hinter dem Schreiber liegen (erste %f)", dst[0]);
      for (int i = 0; i < 5; i++) { push(500 + (uint64_t)i * 128, 128); got = ReadExact(r, m.h->inW, InRing(m.h), 8192, dst.data(), n, target, 2048); CHECK(got == n, "Dauerbetrieb"); }
      got = ReadExact(r, m.h->inW, InRing(m.h), 8192, dst.data(), n, target, 2048); got = ReadExact(r, m.h->inW, InRing(m.h), 8192, dst.data(), n, target, 2048); got = ReadExact(r, m.h->inW, InRing(m.h), 8192, dst.data(), n, target, 2048);
      CHECK(r.underruns >= 1, "Unterlauf nicht gezaehlt"); push(2000, 6000); got = ReadExact(r, m.h->inW, InRing(m.h), 8192, dst.data(), n, target, 2048); CHECK(r.drops >= 1 || r.sync, "zu grosser Rueckstand nicht aufgeholt"); }

    printf("5) Parallelbetrieb (Schreiber- und Lese-Thread mit Vollgas): keine Verfaelschung\n");
    { Mem m(1 << 14); Reader r; std::atomic<bool> stop{false}; uint64_t total = 4000000; bool bad = false; uint64_t read = 0, firstIdx = ~0ull;
      std::thread wr([&] { uint64_t f = 0; std::vector<float> b(240 * CH); while (f < total) { size_t n = 240; for (size_t i = 0; i < n; i++) for (int c = 0; c < CH; c++) b[i * CH + c] = Val(f + i, c);
                          if (f - (r.sync ? r.pos : f) > (1u << 13) && !stop) { std::this_thread::yield(); continue; } Write(m.h->outW, OutRing(m.h), m.h->ringFrames, b.data(), n); f += n; } stop = true; });
      std::vector<float> out(1000 * CH); uint64_t expect = 0; bool started = false;
      while (!stop || r.pos < m.h->outW.load()) { size_t got = ReadAll(r, m.h->outW, OutRing(m.h), m.h->ringFrames, out.data(), 1000, 0); if (!got) { std::this_thread::yield(); continue; }
          // erster Frame bestimmt den Start (Position = r.pos - got); danach muss alles lueckenlos aufeinander folgen
          uint64_t base = r.pos - got; if (!started) { expect = base; started = true; firstIdx = base; }
          if (base != expect) { bad = true; break; } for (size_t i = 0; i < got && !bad; i += 53) for (int c = 0; c < CH; c++) if (out[i * CH + c] != Val(base + i, c)) bad = true; expect += got; read += got; }
      wr.join(); CHECK(!bad, "Daten im Parallelbetrieb verfaelscht"); CHECK(r.drops == 0, "unerwartete Drops: %u", r.drops); printf("   %llu Frames parallel gelesen (Start bei %llu), keine Luecke\n", (unsigned long long)read, (unsigned long long)(firstIdx == ~0ull ? 0 : firstIdx)); }

    printf("6) Surround-Zuordnung: jede Vorlage belegt alle 16 Kanaele genau einmal, SMPTE-Namen\n");
    for (int id = 0; id < NLAYOUTS; id++) { const Layout& L = GetLayout(id); int used[16] = {0}; int total = 0;
        for (int g = 0; g < L.ngroups; g++) { CHECK(L.g[g].count == GroupSize(L.g[g].type), "Vorlage %d Gruppe %d: Groesse", id, g); for (int c = L.g[g].first; c < L.g[g].first + L.g[g].count; c++) { if (c < 16) used[c]++; total++; } }
        bool ok = total == 16; for (int c = 0; c < 16; c++) ok &= used[c] == 1; CHECK(ok, "Vorlage %d (%s) belegt die 16 Kanaele nicht genau einmal", id, L.title); printf("   %d: %-34s Kanal 3 = %s, Kanal 16 = %s\n", id, L.title, ChannelName(id, 2).c_str(), ChannelName(id, 15).c_str()); }
    CHECK(ChannelName(1, 3) == "5.1 LFE" && ChannelName(1, 4) == "5.1 Ls" && ChannelName(0, 5) == "Stereo 3 R" && ChannelName(2, 7) == "7.1 Rrs" && ChannelName(6, 8) == "7.1.4 Ltf", "Kanalnamen weichen von SMPTE ab");

    printf("7) Stereo-Downmix (BS.775, -3 dB): Mitte -> beide Seiten, LFE stumm, Stereo unveraendert\n");
    { float in[16] = {0}, o[2]; in[2] = 1.0f; DownmixFrames(in, 1, 0, G_51, o); CHECK(std::fabs(o[0] - 0.5f) < 1e-4 && std::fabs(o[1] - 0.5f) < 1e-4, "5.1 Mitte: %.4f %.4f (erwartet 0,5)", o[0], o[1]);
      std::memset(in, 0, sizeof in); in[3] = 1.0f; DownmixFrames(in, 1, 0, G_51, o); CHECK(o[0] == 0 && o[1] == 0, "LFE darf nicht im Downmix sein");
      std::memset(in, 0, sizeof in); in[0] = 0.8f; in[1] = -0.3f; DownmixFrames(in, 1, 0, G_STEREO, o); CHECK(o[0] == 0.8f && o[1] == -0.3f, "Stereo-Paar muss unveraendert bleiben");
      std::memset(in, 0, sizeof in); in[4] = 1.0f; in[5] = 1.0f; DownmixFrames(in, 1, 0, G_51, o); CHECK(std::fabs(o[0] - 0.5f) < 1e-4 && std::fabs(o[1] - 0.5f) < 1e-4, "5.1 Surround: %.4f %.4f", o[0], o[1]);
      std::memset(in, 0, sizeof in); for (int i = 8; i < 16; i++) in[i] = 0.5f; DownmixFrames(in, 1, 8, G_71, o); CHECK(o[0] > 0 && std::fabs(o[0] - o[1]) < 1e-6, "zweite 7.1-Gruppe (ab Kanal 9) symmetrisch"); }
    printf("8) Herzschlag-Frische\n"); CHECK(Fresh(1000000000ull, 1500000000ull) && !Fresh(1000000000ull, 3000000000ull) && !Fresh(0, 5), "Frische-Pruefung");
    printf(fails ? "\n%d FEHLER\n" : "\nalles bestanden\n", fails); return fails ? 1 : 0;
}
