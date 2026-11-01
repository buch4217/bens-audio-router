// asio_host.cpp – Test-Host fuer den ASIO-Treiber "Bens Virtual Bus": verhaelt sich wie eine DAW und simuliert parallel die Router-Seite.
// Lauf (Linux, mit Wine):  x86_64-w64-mingw32-g++-posix -std=c++17 -O2 -static asio_host.cpp -o asio_host.exe -lole32 -luuid -ladvapi32 -lwinmm
//                          wine64 asio_host.exe ../driver/bensasio.dll
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <vector>
#include "../driver/asio_iface.h"
#include "../src/vbus_win.h"
using namespace asio;
static const GUID CLSID_Bens = {0xA5C3F1D2, 0x6B8E, 0x4F37, {0x9D, 0x40, 0x2E, 0x7B, 0x1C, 0x9A, 0x5D, 0x88}};
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FEHLER: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static inline float V(uint64_t k, int c) { return (float)(k * 16 + (uint64_t)c); }   // Muster: exakt in float bis k < 1 Mio

// ---- "DAW"-Seite
static const long NBUF = 256; static const int NCH = 16;
static float* g_out[NCH][2]; static float* g_in[NCH][2];
static std::atomic<uint64_t> g_outFrame{0}, g_cbCount{0}, g_inCont{0}, g_inBreak{0}, g_inBad{0}, g_inZero{0}; static uint64_t g_prevIn = ~0ull;
static void BufferSwitch(long idx, Bool) {
    uint64_t k0 = g_outFrame.load();
    for (int c = 0; c < NCH; c++) for (long i = 0; i < NBUF; i++) g_out[c][idx][i] = V(k0 + (uint64_t)i, c);   // DAW-Ausgabe: bekanntes Muster
    g_outFrame += (uint64_t)NBUF;
    // Eingaenge pruefen: Router-Muster, Kanal 0 liefert Frame-Nummer*16
    for (long i = 0; i < NBUF; i++) {
        float v0 = g_in[0][idx][i];
        if (v0 == 0.0f) { g_inZero++; g_prevIn = ~0ull; continue; }
        uint64_t k = (uint64_t)(v0 / 16.0f); bool ok = true;
        for (int c = 1; c < NCH; c++) if (g_in[c][idx][i] != V(k, c)) ok = false;
        if (!ok) { g_inBad++; g_prevIn = ~0ull; continue; }
        if (g_prevIn != ~0ull && k != g_prevIn + 1) g_inBreak++; else g_inCont++;
        g_prevIn = k;
    }
    g_cbCount++;
}
static void RateChanged(SampleRate) {}
static long Message(long sel, long, void*, double*) { return sel == kSelectorSupported ? 0 : sel == kEngineVersion ? 2 : 0; }
static Time* TimeInfoCb(Time* t, long, Bool) { return t; }

int main(int argc, char** argv) {
    const char* dll = argc > 1 ? argv[1] : "../driver/bensasio.dll"; timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    printf("1) DLL laden, COM-Klassenfabrik, Treiber anlegen\n");
    HMODULE m = LoadLibraryA(dll); CHECK(m != nullptr, "DLL nicht ladbar: %s (Fehler %lu)", dll, GetLastError()); if (!m) return 1;
    typedef HRESULT(WINAPI * GCO)(REFCLSID, REFIID, void**); GCO gco = (GCO)GetProcAddress(m, "DllGetClassObject"); CHECK(gco != nullptr, "DllGetClassObject fehlt"); if (!gco) return 1;
    IClassFactory* cf = nullptr; HRESULT hr = gco(CLSID_Bens, IID_IClassFactory, (void**)&cf); CHECK(SUCCEEDED(hr) && cf, "Klassenfabrik: 0x%lx", (unsigned long)hr); if (!cf) return 1;
    IAsio* drv = nullptr; hr = cf->CreateInstance(nullptr, CLSID_Bens, (void**)&drv);   // ASIO-Konvention: IID == CLSID
    CHECK(SUCCEEDED(hr) && drv, "CreateInstance(IID=CLSID): 0x%lx", (unsigned long)hr); if (!drv) return 1;
    GUID other = {0x12345678, 1, 2, {3, 4, 5, 6, 7, 8, 9, 10}}; IUnknown* bad = nullptr; CHECK(cf->CreateInstance(nullptr, other, (void**)&bad) == E_NOINTERFACE, "fremde IID muss abgelehnt werden");
    CHECK(drv->init(nullptr) == TRUE_, "init() fehlgeschlagen");
    char name[64] = {0}; drv->getDriverName(name); printf("   Treiber: %s, Version 0x%lx\n", name, drv->getDriverVersion()); CHECK(strcmp(name, "Bens Virtual Bus") == 0, "Name");

    printf("2) Eigenschaften: Kanaele, Puffergroessen, Raten, Uhr\n");
    long ni = 0, no = 0; drv->getChannels(&ni, &no); CHECK(ni == 16 && no == 16, "Kanaele %ld/%ld", ni, no);
    long mn, mx, pref, gran; drv->getBufferSize(&mn, &mx, &pref, &gran); CHECK(mn == 32 && mx == 2048 && pref == 256 && gran == -1, "Puffer %ld %ld %ld %ld", mn, mx, pref, gran);
    for (double r : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 352800.0, 384000.0}) CHECK(drv->canSampleRate(r) == ASE_OK, "Rate %.0f", r);
    CHECK(drv->canSampleRate(8000.0) != ASE_OK && drv->canSampleRate(768000.0) != ASE_OK, "ungueltige Raten muessen abgelehnt werden");
    ClockSource cs[2]; long nc = 2; drv->getClockSources(cs, &nc); CHECK(nc == 1 && cs[0].isCurrentSource, "Uhr");
    CHECK(drv->setSampleRate(384000.0) == ASE_OK, "setSampleRate 384k"); CHECK(drv->setSampleRate(96000.0) == ASE_OK, "setSampleRate"); double sr = 0; drv->getSampleRate(&sr); CHECK(sr == 96000.0, "Rate nicht uebernommen: %.0f", sr);
    CHECK(drv->setSampleRate(48000.0) == ASE_OK, "setSampleRate 48k");

    printf("3) Brueckenseite (Router-Simulation): Kopf, Surround-Namen\n");
    vbus::Header* H = nullptr; HANDLE hm = nullptr; CHECK(vbus_win::Attach(&H, &hm), "Router kann Bruecke nicht anlegen"); if (!H) return 1;
    CHECK(H->rate.load() == 48000, "Rate in der Bruecke: %u", (unsigned)H->rate.load());
    H->layout.store(1); ChannelInfo ci{}; ci.channel = 2; ci.isInput = FALSE_; drv->getChannelInfo(&ci); printf("   Kanal 3 (Layout 5.1): '%s', Gruppe %ld, Typ %ld\n", ci.name, ci.channelGroup, ci.type);
    CHECK(strcmp(ci.name, "3: 5.1 C") == 0 && ci.type == ST_Float32LSB && ci.channelGroup == 0, "Kanalname/-typ");
    ci.channel = 15; ci.isInput = TRUE_; drv->getChannelInfo(&ci); CHECK(strcmp(ci.name, "16: Stereo 8 R") == 0, "Kanal 16: '%s'", ci.name);
    H->layout.store(0);

    printf("4) Puffer anlegen und 4 s lang laufen (DAW-Ausgabe -> Router, Router -> DAW-Eingang)\n");
    BufferInfo bi[32]; for (int c = 0; c < NCH; c++) { bi[c] = {FALSE_, c, {nullptr, nullptr}}; bi[16 + c] = {TRUE_, c, {nullptr, nullptr}}; }
    Callbacks cb = {BufferSwitch, RateChanged, Message, TimeInfoCb};
    CHECK(drv->createBuffers(bi, 32, 100, &cb) == ASE_InvalidParameter, "Puffergroesse 100 (keine Zweierpotenz) muss abgelehnt werden");
    CHECK(drv->createBuffers(bi, 32, NBUF, &cb) == ASE_OK, "createBuffers"); for (int c = 0; c < NCH; c++) for (int b = 0; b < 2; b++) { g_out[c][b] = (float*)bi[c].buffers[b]; g_in[c][b] = (float*)bi[16 + c].buffers[b]; CHECK(bi[c].buffers[b] && bi[16 + c].buffers[b], "Puffer-Zeiger %d/%d", c, b); }
    CHECK(H->bufFrames.load() == (uint32_t)NBUF, "Puffergroesse in der Bruecke: %u", (unsigned)H->bufFrames.load());
    std::atomic<bool> stop{false}; std::atomic<uint64_t> rOut{0}, rBad{0}, rBreak{0}; std::atomic<bool> sawDriver{false};
    std::thread router([&] {   // Router-Simulation: liest OUT-Ring (DAW-Ausgabe), schreibt IN-Ring (Router-Signale) nach Uhr
        vbus::Reader rd; rd.sync = true; rd.pos = 0; std::vector<float> buf((size_t)4096 * 16), w(1024 * 16); uint64_t wk = 0; LARGE_INTEGER f, t0; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); uint64_t expect = 0; bool first = true;
        while (!stop) {
            H->routerOn.store(1); H->routerBeatNs.store(vbus_win::NowNs()); if (H->driverOn.load() && vbus::Fresh(H->driverBeatNs.load(), vbus_win::NowNs())) sawDriver = true;
            size_t got; while ((got = vbus::ReadAll(rd, H->outW, vbus::OutRing(H), H->ringFrames, buf.data(), 4096, 1 << 15)) > 0) {
                uint64_t base = rd.pos - got; if (first) { expect = base; first = false; } if (base != expect) rBreak++; expect = base + got;
                for (size_t i = 0; i < got; i++) for (int c = 0; c < 16; c++) if (buf[i * 16 + (size_t)c] != V(base + i, c)) rBad++;
                rOut += got; if (got < 4096) break; }
            if (H->driverOn.load()) { LARGE_INTEGER n; QueryPerformanceCounter(&n); uint64_t due = (uint64_t)((double)(n.QuadPart - t0.QuadPart) / (double)f.QuadPart * 48000.0) + 1;
                while (wk < due) { size_t blk = (size_t)std::min<uint64_t>(due - wk, 512); for (size_t i = 0; i < blk; i++) for (int c = 0; c < 16; c++) w[i * 16 + (size_t)c] = V(wk + 1 + i, c); vbus::Write(H->inW, vbus::InRing(H), H->ringFrames, w.data(), blk); wk += blk; } }
            else QueryPerformanceCounter(&t0);
            Sleep(1);
        }
    });
    Sleep(300); CHECK(drv->start() == ASE_OK, "start()"); DWORD t1 = GetTickCount(); Sleep(4000); Samples sp; TimeStamp ts; drv->getSamplePosition(&sp, &ts);
    double secs = (GetTickCount() - t1) / 1000.0; uint64_t pos = ((uint64_t)sp.hi << 32) | sp.lo; CHECK(drv->stop() == ASE_OK, "stop()"); Sleep(200); stop = true; router.join();
    double expectCb = secs * 48000.0 / NBUF; printf("   Callbacks: %llu (erwartet ~%.0f), Samplepos %llu\n", (unsigned long long)g_cbCount.load(), expectCb, (unsigned long long)pos);
    CHECK(g_cbCount > expectCb * 0.8 && g_cbCount < expectCb * 1.2, "Takt weicht ab: %llu Callbacks statt ~%.0f", (unsigned long long)g_cbCount.load(), expectCb);
    CHECK(sawDriver, "Router hat den Treiber nicht als aktiv erkannt (Herzschlag)");
    printf("   DAW -> Router: %llu Frames bit-genau gepruefte Kanaele x16, falsche Samples %llu, Spruenge %llu\n", (unsigned long long)rOut.load(), (unsigned long long)rBad.load(), (unsigned long long)rBreak.load());
    CHECK(rOut.load() >= (uint64_t)NBUF * g_cbCount.load() * 9 / 10 && rBad == 0 && rBreak == 0, "DAW->Router: %llu Frames, falsch %llu, Spruenge %llu", (unsigned long long)rOut.load(), (unsigned long long)rBad.load(), (unsigned long long)rBreak.load());
    printf("   Router -> DAW: lueckenlos %llu Frames, Spruenge %llu, verfaelscht %llu, Stille %llu\n", (unsigned long long)g_inCont.load(), (unsigned long long)g_inBreak.load(), (unsigned long long)g_inBad.load(), (unsigned long long)g_inZero.load());
    CHECK(g_inBad == 0, "Router->DAW: %llu verfaelschte Frames", (unsigned long long)g_inBad.load()); CHECK(g_inCont.load() > (uint64_t)NBUF * g_cbCount.load() / 2, "Router->DAW: zu wenig lueckenlose Frames (%llu)", (unsigned long long)g_inCont.load());
    CHECK(H->driverOn.load() == 0, "nach stop() muss driverOn 0 sein");
    CHECK(drv->disposeBuffers() == ASE_OK, "disposeBuffers"); drv->Release(); cf->Release();

    printf("5) Registrierung (DllRegisterServer/DllUnregisterServer)\n");
    typedef HRESULT(WINAPI * RS)(); RS reg = (RS)GetProcAddress(m, "DllRegisterServer"), unreg = (RS)GetProcAddress(m, "DllUnregisterServer");
    CHECK(reg && SUCCEEDED(reg()), "DllRegisterServer fehlgeschlagen"); HKEY k; bool found = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO\\Bens Virtual Bus", 0, KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS;
    char val[100] = {0}; DWORD n = sizeof val; if (found) { RegQueryValueExA(k, "CLSID", nullptr, nullptr, (BYTE*)val, &n); RegCloseKey(k); } printf("   HKLM\\SOFTWARE\\ASIO\\Bens Virtual Bus -> CLSID %s\n", val);
    CHECK(found && strcmp(val, "{A5C3F1D2-6B8E-4F37-9D40-2E7B1C9A5D88}") == 0, "ASIO-Eintrag fehlt oder falsche CLSID"); CHECK(unreg && SUCCEEDED(unreg()), "DllUnregisterServer");
    CHECK(RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO\\Bens Virtual Bus", 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS, "ASIO-Eintrag nach Unregister noch vorhanden");
    vbus_win::Detach(&H, &hm); printf(fails ? "\n%d FEHLER\n" : "\nalles bestanden\n", fails); return fails ? 1 : 0;
}
