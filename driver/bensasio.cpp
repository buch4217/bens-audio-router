// bensasio.cpp – "Bens Virtual Bus": ASIO-Treiber (x64, MIT) mit 16 Ein- und 16 Ausgaengen = 8 Stereo-Paare
// (© Ben Buchholz · Buccloud Software (DE))
//
// * Ausgaenge der DAW (16 Kanaele) -> Router ("Virtuelle Eingaenge"), Eingaenge der DAW (16 Kanaele) <- Router ("Virtuelle Ausgaenge").
// * Transport: gemeinsamer Speicher "Local\BensVBus1" (siehe ../src/vbus.h). Der Takt kommt vom Treiber (QPC, hochaufloesender Timer, MMCSS "Pro Audio").
// * Eigenstaendig: reiner User-Mode-COM-Server (DLL), keine Kernel-Komponente, keine Signatur noetig. Registrierung per regsvr32 / DllRegisterServer.
// * Kanalnamen folgen der im Router gewaehlten Surround-Zuordnung (SMPTE-Namen, z.B. "5.1 C", "7.1.4 Ltf").
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <unknwn.h>
#include <mmsystem.h>
#include <avrt.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "asio_iface.h"
#include "../src/vbus_win.h"

using namespace asio;
// {A5C3F1D2-6B8E-4F37-9D40-2E7B1C9A5D88}
static const GUID CLSID_BensAsio = {0xA5C3F1D2, 0x6B8E, 0x4F37, {0x9D, 0x40, 0x2E, 0x7B, 0x1C, 0x9A, 0x5D, 0x88}};
static const wchar_t* DRV_NAME = L"Bens Virtual Bus";
static HMODULE g_mod = nullptr;
static std::atomic<long> g_objs{0}, g_locks{0};

class BensAsio : public IAsio {
public:
    BensAsio() { g_objs++; }
    // ---- IUnknown (ASIO-Hosts fragen mit IID == CLSID)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
        if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, CLSID_BensAsio)) { *o = static_cast<IAsio*>(this); AddRef(); return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { long r = --ref_; if (r == 0) delete this; return (ULONG)r; }
    virtual ~BensAsio() { stop(); disposeBuffers(); vbus_win::Detach(&h_, &map_); g_objs--; }
    // ---- IAsio
    Bool init(void*) override {
        if (!h_ && !vbus_win::Attach(&h_, &map_)) { err_ = "Brücke nicht erreichbar"; return FALSE_; }
        h_->driverVersion.store(1);
        rate_ = (double)h_->rate.load(); if (rate_ < 8000) rate_ = 48000;
        return TRUE_;
    }
    void getDriverName(char* n) override { std::snprintf(n, 32, "Bens Virtual Bus"); }
    long getDriverVersion() override { return 0x00010000; }
    void getErrorMessage(char* s) override { std::snprintf(s, 124, "%s", err_.c_str()); }
    Error start() override {
        if (!h_ || !cb_ || running_) return running_ ? ASE_OK : ASE_NotPresent;
        pos_ = 0; stopFlag_ = false; inRd_.reset(); h_->bufFrames.store((uint32_t)buf_); h_->epoch.fetch_add(1); h_->driverOn.store(1);
        thr_ = CreateThread(nullptr, 0, &BensAsio::ThreadMain, this, 0, nullptr); if (!thr_) { h_->driverOn.store(0); return ASE_HWMalfunction; }
        running_ = true; return ASE_OK;
    }
    Error stop() override {
        if (!running_) return ASE_OK;
        stopFlag_ = true; if (thr_) { WaitForSingleObject(thr_, 3000); CloseHandle(thr_); thr_ = nullptr; }
        running_ = false; if (h_) { h_->driverOn.store(0); h_->epoch.fetch_add(1); } return ASE_OK;
    }
    Error getChannels(long* i, long* o) override { *i = vbus::CH; *o = vbus::CH; return ASE_OK; }
    Error getLatencies(long* il, long* ol) override { long b = buf_ ? buf_ : 256; *il = b + (long)(rate_ * 0.004) + b; *ol = b; return ASE_OK; }
    Error getBufferSize(long* mn, long* mx, long* pref, long* gran) override { *mn = 32; *mx = 2048; *pref = 256; *gran = -1; return ASE_OK; }
    Error canSampleRate(SampleRate r) override { static const double ok[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000}; for (double x : ok) if (std::fabs(r - x) < 0.5) return ASE_OK; return ASE_NoClock; }
    Error getSampleRate(SampleRate* r) override { *r = rate_; return ASE_OK; }
    Error setSampleRate(SampleRate r) override {
        if (canSampleRate(r) != ASE_OK) return ASE_NoClock;
        if (std::fabs(r - rate_) > 0.5) { rate_ = r; if (h_) { h_->rate.store((uint32_t)r); h_->epoch.fetch_add(1); } if (cb_ && cb_->sampleRateDidChange) cb_->sampleRateDidChange(r); }
        return ASE_OK;
    }
    Error getClockSources(ClockSource* c, long* n) override { if (*n < 1) return ASE_InvalidParameter; c[0] = {}; c[0].index = 0; c[0].associatedChannel = -1; c[0].associatedGroup = -1; c[0].isCurrentSource = TRUE_; std::snprintf(c[0].name, 32, "Interne Uhr (Systemzeit)"); *n = 1; return ASE_OK; }
    Error setClockSource(long ref) override { return ref == 0 ? ASE_OK : ASE_InvalidParameter; }
    Error getSamplePosition(Samples* s, TimeStamp* t) override {
        uint64_t p = pos_.load(), ns = tsNs_.load(); s->hi = (unsigned long)(p >> 32); s->lo = (unsigned long)(p & 0xFFFFFFFFu); t->hi = (unsigned long)(ns >> 32); t->lo = (unsigned long)(ns & 0xFFFFFFFFu); return running_ ? ASE_OK : ASE_SPNotAdvancing;
    }
    Error getChannelInfo(ChannelInfo* i) override {
        if (i->channel < 0 || i->channel >= vbus::CH) return ASE_InvalidParameter;
        int ly = h_ ? (int)h_->layout.load() : 0; const vbus::Layout& L = vbus::GetLayout(ly); int grp = 0;
        for (int g = 0; g < L.ngroups; g++) if (i->channel >= L.g[g].first && i->channel < L.g[g].first + L.g[g].count) grp = g;
        i->isActive = active_[i->isInput ? 1 : 0][i->channel] ? TRUE_ : FALSE_; i->channelGroup = grp; i->type = ST_Float32LSB;
        std::snprintf(i->name, 32, "%ld: %s", i->channel + 1, vbus::ChannelName(ly, i->channel).c_str()); return ASE_OK;
    }
    Error createBuffers(BufferInfo* infos, long n, long size, Callbacks* cb) override {
        if (!h_ || !cb || n < 1 || n > 2 * vbus::CH || size < 32 || size > 2048 || (size & (size - 1))) return ASE_InvalidParameter;
        disposeBuffers(); buf_ = size; cb_ = cb; std::memset(active_, 0, sizeof active_);
        mem_.assign((size_t)n * 2 * (size_t)size, 0.0f);
        for (long k = 0; k < n; k++) {
            if (infos[k].channelNum < 0 || infos[k].channelNum >= vbus::CH) { mem_.clear(); return ASE_InvalidParameter; }
            infos[k].buffers[0] = &mem_[((size_t)k * 2 + 0) * (size_t)size]; infos[k].buffers[1] = &mem_[((size_t)k * 2 + 1) * (size_t)size];
            active_[infos[k].isInput ? 1 : 0][infos[k].channelNum] = true; ch_.push_back({infos[k].isInput != 0, (int)infos[k].channelNum, (int)k});
        }
        // Anzeige in der Bruecke
        h_->bufFrames.store((uint32_t)size); h_->rate.store((uint32_t)rate_); h_->epoch.fetch_add(1); return ASE_OK;
    }
    Error disposeBuffers() override { stop(); ch_.clear(); mem_.clear(); cb_ = nullptr; buf_ = 0; std::memset(active_, 0, sizeof active_); return ASE_OK; }
    Error controlPanel() override {
        char t[400]; bool rt = h_ && h_->routerOn.load() && vbus::Fresh(h_->routerBeatNs.load(), vbus_win::NowNs());
        std::snprintf(t, sizeof t, "Bens Virtual Bus\n\nRouter: %s\nSamplerate: %.0f Hz\nPuffer: %ld Frames\nKanäle: 16 Ein / 16 Aus (8 Stereo-Paare)\n\nDie Einstellungen (Surround-Zuordnung, Virtuelle Eingänge/Ausgänge) nimmst du im Bens Audio Router unter „Studio…“ vor.", rt ? "verbunden" : "nicht gestartet (Bens Audio Router starten und Brücke einschalten)", rate_, buf_);
        MessageBoxA(nullptr, t, "Bens Virtual Bus", MB_OK | MB_ICONINFORMATION); return ASE_OK;
    }
    Error future(long, void*) override { return ASE_InvalidParameter; }
    Error outputReady() override { return ASE_NotPresent; }

private:
    struct Ch { bool in; int num; int idx; };
    static DWORD WINAPI ThreadMain(LPVOID p) { static_cast<BensAsio*>(p)->loop(); return 0; }
    void loop() {
        DWORD task = 0; HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task); timeBeginPeriod(1);
        HANDLE tm = CreateWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); auto now = [&] { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart; };
        const long n = buf_; const double rate = rate_, period = (double)n / rate; double next = now() + period; long idx = 0;
        std::vector<float> in16((size_t)n * vbus::CH), out16((size_t)n * vbus::CH);
        const size_t target = (size_t)n + (size_t)(rate * 0.004), maxLag = target + (size_t)n * 4 + (size_t)(rate * 0.02);
        while (!stopFlag_) {
            double t = now();
            if (t < next) {   // bis zum naechsten Puffer warten (grob schlafen, zuletzt kurz aktiv)
                double wait = next - t;
                if (wait > 0.002) { if (tm) { LARGE_INTEGER d; d.QuadPart = -(LONGLONG)((wait - 0.0008) * 1e7); SetWaitableTimer(tm, &d, 0, nullptr, nullptr, FALSE); WaitForSingleObject(tm, 50); } else Sleep((DWORD)((wait - 0.001) * 1000)); }
                else if (wait > 0.0003) Sleep(0);
                continue;
            }
            if (t - next > period * 3) next = t;   // weit zurueck: nicht aufholen
            next += period;
            // 1) Router -> DAW: Eingangspuffer fuellen
            vbus::ReadExact(inRd_, h_->inW, vbus::InRing(h_), h_->ringFrames, in16.data(), (size_t)n, target, maxLag);
            for (const Ch& c : ch_) if (c.in) { float* d = (float*)bufPtr(c.idx, idx); for (long i = 0; i < n; i++) d[i] = in16[(size_t)i * vbus::CH + (size_t)c.num]; }
            // 2) Host aufrufen
            tsNs_ = vbus_win::NowNs(); h_->driverBeatNs.store(tsNs_.load()); h_->rate.store((uint32_t)rate);
            if (cb_ && cb_->bufferSwitch) cb_->bufferSwitch(idx, TRUE_);
            // 3) DAW -> Router: Ausgangspuffer einsammeln
            std::fill(out16.begin(), out16.end(), 0.0f);
            for (const Ch& c : ch_) if (!c.in) { const float* s = (const float*)bufPtr(c.idx, idx); for (long i = 0; i < n; i++) out16[(size_t)i * vbus::CH + (size_t)c.num] = s[i]; }
            vbus::Write(h_->outW, vbus::OutRing(h_), h_->ringFrames, out16.data(), (size_t)n);
            pos_ += (uint64_t)n; idx ^= 1;
        }
        if (tm) CloseHandle(tm);
        timeEndPeriod(1);
        if (mm) AvRevertMmThreadCharacteristics(mm);
    }
    void* bufPtr(int k, long idx) { return &mem_[((size_t)k * 2 + (size_t)idx) * (size_t)buf_]; }

    std::atomic<long> ref_{1}; vbus::Header* h_ = nullptr; HANDLE map_ = nullptr; Callbacks* cb_ = nullptr;
    long buf_ = 0; double rate_ = 48000; bool active_[2][vbus::CH] = {}; std::vector<float> mem_; std::vector<Ch> ch_;
    std::atomic<uint64_t> pos_{0}, tsNs_{0}; HANDLE thr_ = nullptr; std::atomic<bool> stopFlag_{false}; bool running_ = false; vbus::Reader inRd_; std::string err_ = "";
};

// ---- COM-Klassenfabrik
class Factory : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
        if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, IID_IClassFactory)) { *o = this; AddRef(); return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID r, void** o) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        BensAsio* a = new BensAsio(); HRESULT hr = a->QueryInterface(r, o); a->Release(); return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL l) override { l ? g_locks++ : g_locks--; return S_OK; }
};
static Factory g_factory;

static std::wstring GuidStr() { wchar_t b[64]; StringFromGUID2(CLSID_BensAsio, b, 64); return b; }
static bool SetKey(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& v) {
    HKEY k; if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    bool ok = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS; RegCloseKey(k); return ok;
}

extern "C" {
BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID) { if (why == DLL_PROCESS_ATTACH) { g_mod = h; DisableThreadLibraryCalls(h); } return TRUE; }
__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID r, void** o) { if (!IsEqualCLSID(c, CLSID_BensAsio)) { *o = nullptr; return CLASS_E_CLASSNOTAVAILABLE; } return g_factory.QueryInterface(r, o); }
__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow() { return g_objs == 0 && g_locks == 0 ? S_OK : S_FALSE; }
__declspec(dllexport) HRESULT WINAPI DllRegisterServer() {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(g_mod, path, MAX_PATH); std::wstring g = GuidStr();
    bool ok = SetKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\" + g, nullptr, DRV_NAME) && SetKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\" + g + L"\\InprocServer32", nullptr, path)
           && SetKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\" + g + L"\\InprocServer32", L"ThreadingModel", L"Both")
           && SetKey(HKEY_LOCAL_MACHINE, std::wstring(L"SOFTWARE\\ASIO\\") + DRV_NAME, L"CLSID", g) && SetKey(HKEY_LOCAL_MACHINE, std::wstring(L"SOFTWARE\\ASIO\\") + DRV_NAME, L"Description", L"Bens Virtual Bus (16 Kanäle, 8 Stereo-Paare)");
    return ok ? S_OK : (HRESULT)0x80040201L;   // SELFREG_E_CLASS
}
__declspec(dllexport) HRESULT WINAPI DllUnregisterServer() {
    std::wstring g = GuidStr(); RegDeleteTreeW(HKEY_LOCAL_MACHINE, (L"SOFTWARE\\Classes\\CLSID\\" + g).c_str()); RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\ASIO\\") + DRV_NAME).c_str()); return S_OK;
}
}
