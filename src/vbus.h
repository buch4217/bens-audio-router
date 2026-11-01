// vbus.h – "Bens Virtual Bus": 16 Kanaele (= 8 virtuelle Stereo-Eingaenge) zwischen DAW/Treiber und Router
// (MIT, © Ben Buchholz · Buccloud Software (DE))
//
// Gemeinsamer Speicher (Windows: benannte Dateiabbildung "Local\BensVBus1"), lock-frei, zwei Ringe:
//   OUT-Ring: DAW -> Router   (die 16 ASIO-Ausgaenge des Treibers; im Router die 8 "Virtuellen Eingaenge")
//   IN-Ring : Router -> DAW   (die 16 ASIO-Eingaenge des Treibers; im Router die 8 "Virtuellen Ausgaenge")
// Beide Ringe: Frames mit je 16 float-Kanaelen, interleaved, Zweierpotenz an Frames. Der Schreibzaehler ist ein 64-Bit-Frame-Zaehler.
// Takt: Treiber (Host-Puffer) und Router laufen beide auf der Systemuhr (QPC) -> kein Auseinanderdriften, nur Jitter.
//
// Surround-Zuordnung: Die 16 Kanaele sind je nach "Layout" als Stereo-Paare oder Surround-Gruppen (5.1/7.1/Quad/7.1.4) benannt
// (SMPTE-Reihenfolge). Der Treiber meldet die Namen an die DAW; der Router bietet fuer jede Gruppe zusaetzlich einen Stereo-Downmix.
// Portabel (nur Speicherzugriff) -> nativ testbar (tests/test_vbus.cpp).
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vbus {

static const uint32_t MAGIC = 0x31425642u;   // "BVB1"
static const uint32_t VERSION = 1;
static const int CH = 16;                    // Kanaele je Ring
static const int PAIRS = 8;                  // Stereo-Paare
static const uint32_t DEFAULT_RING = 1u << 16;   // 65536 Frames (1,4 s bei 48 kHz, 0,34 s bei 192 kHz)
static const size_t HEADER_BYTES = 256;

struct Header {
    uint32_t magic, version, headerSize, ch, ringFrames, reserved0;
    // Router -> Treiber
    std::atomic<uint32_t> routerOn;        // 1 = Router bedient die Bruecke
    std::atomic<uint32_t> layout;          // Surround-Zuordnung (siehe Layouts)
    std::atomic<uint64_t> routerBeatNs;    // Herzschlag (QPC in ns)
    // Treiber -> Router
    std::atomic<uint32_t> driverOn;        // 1 = DAW streamt
    std::atomic<uint32_t> rate;            // Abtastrate der Bruecke (setzt die DAW)
    std::atomic<uint32_t> bufFrames;       // ASIO-Puffergroesse der DAW
    std::atomic<uint64_t> driverBeatNs;
    std::atomic<uint32_t> epoch;           // zaehlt bei Start/Stopp/Ratenwechsel hoch -> Router synchronisiert neu
    std::atomic<uint32_t> driverVersion;
    // Schreibzaehler (je eigene Cacheline)
    alignas(64) std::atomic<uint64_t> outW;   // DAW -> Router: schreibt der Treiber
    alignas(64) std::atomic<uint64_t> inW;    // Router -> DAW: schreibt der Router
};
static_assert(sizeof(Header) <= HEADER_BYTES, "Header zu gross");

inline size_t TotalBytes(uint32_t ringFrames) { return HEADER_BYTES + 2 * (size_t)ringFrames * CH * sizeof(float); }
inline float* OutRing(Header* h) { return reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(h) + HEADER_BYTES); }
inline float* InRing(Header* h) { return OutRing(h) + (size_t)h->ringFrames * CH; }
inline bool Valid(const Header* h) { return h && h->magic == MAGIC && h->version == VERSION && h->ch == (uint32_t)CH && h->ringFrames >= 1024 && (h->ringFrames & (h->ringFrames - 1)) == 0; }
inline void InitHeader(Header* h, uint32_t ringFrames = DEFAULT_RING) {   // Router legt den Block an (Speicher vorher genullt)
    h->magic = MAGIC; h->version = VERSION; h->headerSize = (uint32_t)HEADER_BYTES; h->ch = (uint32_t)CH; h->ringFrames = ringFrames; h->reserved0 = 0;
    h->routerOn.store(0); h->layout.store(0); h->routerBeatNs.store(0); h->driverOn.store(0); h->rate.store(48000); h->bufFrames.store(0);
    h->driverBeatNs.store(0); h->epoch.store(0); h->driverVersion.store(0); h->outW.store(0); h->inW.store(0);
}

// ---------- Ringzugriff (ein Schreiber, ein Leser je Ring)
inline void Write(std::atomic<uint64_t>& w, float* ring, uint32_t rf, const float* src, size_t n) {
    uint64_t p = w.load(std::memory_order_relaxed); size_t i = (size_t)(p & (rf - 1)), first = std::min<size_t>(n, rf - i);
    std::memcpy(ring + i * CH, src, first * CH * sizeof(float));
    if (first < n) std::memcpy(ring, src + first * CH, (n - first) * CH * sizeof(float));
    w.store(p + n, std::memory_order_release);
}
struct Reader {
    uint64_t pos = 0; bool sync = false; uint32_t drops = 0, underruns = 0;
    void reset() { sync = false; }
};
// Router liest den OUT-Ring: moeglichst alles Neue. keep = Frames, die beim (Neu-)Synchronisieren im Puffer bleiben duerfen.
inline size_t ReadAll(Reader& r, const std::atomic<uint64_t>& w, const float* ring, uint32_t rf, float* dst, size_t maxFrames, size_t keep) {
    uint64_t wp = w.load(std::memory_order_acquire);
    if (!r.sync || wp < r.pos) { r.pos = wp > keep ? wp - keep : 0; r.sync = true; }
    if (wp - r.pos > (uint64_t)rf * 3 / 4) { r.pos = wp > keep ? wp - keep : 0; r.drops++; }   // Leser zu langsam gewesen: neu aufsetzen
    size_t n = (size_t)std::min<uint64_t>(maxFrames, wp - r.pos); if (!n) return 0;
    size_t i = (size_t)(r.pos & (rf - 1)), first = std::min<size_t>(n, rf - i);
    std::memcpy(dst, ring + i * CH, first * CH * sizeof(float));
    if (first < n) std::memcpy(dst + first * CH, ring, (n - first) * CH * sizeof(float));
    // der Schreiber darf waehrend des Kopierens nicht ueberholt haben
    if (w.load(std::memory_order_acquire) - r.pos > rf) { r.pos = w.load() - keep; r.drops++; return 0; }
    r.pos += n; return n;
}
// Treiber liest den IN-Ring genau n Frames (Host-Puffer). target = gewuenschter Vorlauf in Frames, maxLag = ab hier werden Frames verworfen.
// Rueckgabe: Anzahl echter Frames (Rest bleibt Stille).
inline size_t ReadExact(Reader& r, const std::atomic<uint64_t>& w, const float* ring, uint32_t rf, float* dst, size_t n, size_t target, size_t maxLag) {
    std::memset(dst, 0, n * CH * sizeof(float));
    uint64_t wp = w.load(std::memory_order_acquire);
    if (!r.sync) { if (wp < target + n) return 0; r.pos = wp - target; r.sync = true; }
    if (wp < r.pos) { r.sync = false; return 0; }
    if (wp - r.pos > maxLag) { r.pos = wp - target; r.drops++; }
    if (wp - r.pos < n) { r.underruns++; r.sync = false; return 0; }   // zu wenig: Stille, beim naechsten Mal neu auf den Vorlauf setzen
    size_t i = (size_t)(r.pos & (rf - 1)), first = std::min<size_t>(n, rf - i);
    std::memcpy(dst, ring + i * CH, first * CH * sizeof(float));
    if (first < n) std::memcpy(dst + first * CH, ring, (n - first) * CH * sizeof(float));
    r.pos += n; return n;
}

// ---------- Surround-Zuordnung (SMPTE-Reihenfolge)
enum GroupType { G_STEREO = 0, G_QUAD = 1, G_51 = 2, G_71 = 3, G_714 = 4 };
struct Group { int first, count, type; const char* name; };
struct Layout { const char* title; int ngroups; Group g[8]; };
static const int NLAYOUTS = 7;
inline const Layout& GetLayout(int id) {
    static const Layout L[NLAYOUTS] = {
        {"8 \xC3\x97 Stereo", 8, {{0, 2, G_STEREO, "Stereo 1"}, {2, 2, G_STEREO, "Stereo 2"}, {4, 2, G_STEREO, "Stereo 3"}, {6, 2, G_STEREO, "Stereo 4"}, {8, 2, G_STEREO, "Stereo 5"}, {10, 2, G_STEREO, "Stereo 6"}, {12, 2, G_STEREO, "Stereo 7"}, {14, 2, G_STEREO, "Stereo 8"}}},
        {"5.1 + 5 \xC3\x97 Stereo", 6, {{0, 6, G_51, "5.1"}, {6, 2, G_STEREO, "Stereo 4"}, {8, 2, G_STEREO, "Stereo 5"}, {10, 2, G_STEREO, "Stereo 6"}, {12, 2, G_STEREO, "Stereo 7"}, {14, 2, G_STEREO, "Stereo 8"}}},
        {"7.1 + 4 \xC3\x97 Stereo", 5, {{0, 8, G_71, "7.1"}, {8, 2, G_STEREO, "Stereo 5"}, {10, 2, G_STEREO, "Stereo 6"}, {12, 2, G_STEREO, "Stereo 7"}, {14, 2, G_STEREO, "Stereo 8"}}},
        {"5.1 \xC3\x97 2 + 2 \xC3\x97 Stereo", 4, {{0, 6, G_51, "5.1 A"}, {6, 6, G_51, "5.1 B"}, {12, 2, G_STEREO, "Stereo 7"}, {14, 2, G_STEREO, "Stereo 8"}}},
        {"7.1 \xC3\x97 2", 2, {{0, 8, G_71, "7.1 A"}, {8, 8, G_71, "7.1 B"}}},
        {"Quad \xC3\x97 4", 4, {{0, 4, G_QUAD, "Quad 1"}, {4, 4, G_QUAD, "Quad 2"}, {8, 4, G_QUAD, "Quad 3"}, {12, 4, G_QUAD, "Quad 4"}}},
        {"7.1.4 (Atmos-Bett) + 2 \xC3\x97 Stereo", 3, {{0, 12, G_714, "7.1.4"}, {12, 2, G_STEREO, "Stereo 7"}, {14, 2, G_STEREO, "Stereo 8"}}},
    };
    return L[id < 0 || id >= NLAYOUTS ? 0 : id];
}
inline int GroupSize(int type) { static const int s[] = {2, 4, 6, 8, 12}; return s[type]; }
inline const char* GroupChannelName(int type, int i) {
    static const char* st[] = {"L", "R"}; static const char* qd[] = {"L", "R", "Ls", "Rs"};
    static const char* s51[] = {"L", "R", "C", "LFE", "Ls", "Rs"}; static const char* s71[] = {"L", "R", "C", "LFE", "Ls", "Rs", "Lrs", "Rrs"};
    static const char* s714[] = {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs", "Ltf", "Rtf", "Ltr", "Rtr"};
    const char* const* t = type == G_STEREO ? st : type == G_QUAD ? qd : type == G_51 ? s51 : type == G_71 ? s71 : s714; return t[i];
}
// Name eines der 16 Kanaele, z.B. "5.1 C" oder "Stereo 4 L" (fuer die DAW und den Router)
inline std::string ChannelName(int layout, int ch) {
    const Layout& L = GetLayout(layout);
    for (int g = 0; g < L.ngroups; g++) if (ch >= L.g[g].first && ch < L.g[g].first + L.g[g].count) return std::string(L.g[g].name) + " " + GroupChannelName(L.g[g].type, ch - L.g[g].first);
    return "Kanal " + std::to_string(ch + 1);
}
// Stereo-Downmix-Koeffizienten (ITU-R BS.775, -3 dB gesamt; LFE nicht im Downmix) fuer einen Gruppentyp: je Kanal (L', R')
struct Mix { float l, r; };
inline int DownmixCoefs(int type, Mix* m) {
    const float c = 0.70710678f, g = 0.70710678f;   // Mitte/Surround -3 dB, Gesamtpegel -3 dB
    int n = GroupSize(type);
    for (int i = 0; i < n; i++) m[i] = {0, 0};
    switch (type) {
        case G_STEREO: m[0] = {1, 0}; m[1] = {0, 1}; return n;   // unveraendert (ohne -3 dB)
        case G_QUAD: m[0] = {g, 0}; m[1] = {0, g}; m[2] = {g * c, 0}; m[3] = {0, g * c}; return n;
        case G_51: m[0] = {g, 0}; m[1] = {0, g}; m[2] = {g * c, g * c}; m[3] = {0, 0}; m[4] = {g * c, 0}; m[5] = {0, g * c}; return n;
        case G_71: m[0] = {g, 0}; m[1] = {0, g}; m[2] = {g * c, g * c}; m[3] = {0, 0}; m[4] = {g * c, 0}; m[5] = {0, g * c}; m[6] = {g * 0.5f, 0}; m[7] = {0, g * 0.5f}; return n;
        default: m[0] = {g, 0}; m[1] = {0, g}; m[2] = {g * c, g * c}; m[3] = {0, 0}; m[4] = {g * c, 0}; m[5] = {0, g * c}; m[6] = {g * 0.5f, 0}; m[7] = {0, g * 0.5f};
                 m[8] = {g * 0.5f, 0}; m[9] = {0, g * 0.5f}; m[10] = {g * 0.35f, 0}; m[11] = {0, g * 0.35f}; return n;
    }
}
// Downmix einer Gruppe aus 16-Kanal-Frames (interleaved) in Stereo-Frames
inline void DownmixFrames(const float* in16, size_t frames, int first, int type, float* outLR) {
    Mix m[12]; int n = DownmixCoefs(type, m);
    for (size_t f = 0; f < frames; f++) {
        float l = 0, r = 0; const float* p = in16 + f * CH + first;
        for (int i = 0; i < n; i++) { l += p[i] * m[i].l; r += p[i] * m[i].r; }
        outLR[2 * f] = l; outLR[2 * f + 1] = r;
    }
}

// Zeit: Herzschlag-Auswertung (ns, QPC)
inline bool Fresh(uint64_t beatNs, uint64_t nowNs, double maxAgeSec = 0.75) { return beatNs != 0 && nowNs >= beatNs && (double)(nowNs - beatNs) * 1e-9 < maxAgeSec; }

}  // namespace vbus
