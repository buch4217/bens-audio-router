// Bens Audio Router 3.0
// Node-basierter, audiophiler Audio-Router (WASAPI) mit Sonos-Stream-Ausgang (UPnP + WAV),
// Sinc-Resampling, TPDF-Dither, Auto-Sync, Layout-Verwaltung und nativem Dark-Mode.
// Nur Windows-Bordmittel (WASAPI, Winsock, Win32/GDI). Keine Fremdbibliotheken.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <initguid.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core.h"
#include "calib.h"
#include "stream.h"
#include "airplay.h"
#include "wavrec.h"
#include "route.h"
#include "qr.h"
#include "webwin.h"
#include "web_assets.h"
#include "ui.h"

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

// ======================================================================= Basis

#define APP_NAME      L"Bens Audio Router"
#define APP_VERSION   L"4.5"
#define APP_PUBLISHER L"Buccloud Software (DE)"
#define APP_COPYRIGHT L"© Ben Buchholz · www.buccloud.de · 2026"
#define APP_URL       L"https://www.buccloud.de"

static LONGLONG g_qpf = 1;
double NowSec() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)g_qpf;
}

static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logCs;
static double g_logT0 = 0;
void Log(const char* fmt, ...) {
    if (!g_log) return;
    char b[800];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_logCs);
    fprintf(g_log, "[%8.3f] %s\n", NowSec() - g_logT0, b);
    fflush(g_log);
    LeaveCriticalSection(&g_logCs);
}

template <class T>
struct Com {
    T* p = nullptr;
    Com() {}
    ~Com() { if (p) p->Release(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T** operator&() { return &p; }
    T* operator->() { return p; }
    operator T*() { return p; }
};

static std::wstring HrMsg(const wchar_t* what, HRESULT hr) {
    wchar_t b[300];
    swprintf(b, 300, L"%ls (0x%08lX)", what, (unsigned long)hr);
    return b;
}

static std::wstring AppDir() {
    wchar_t p[MAX_PATH] = {};
    GetEnvironmentVariableW(L"APPDATA", p, MAX_PATH);
    std::wstring dir = std::wstring(p) + L"\\BensAudioRouter";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}
static std::wstring LayoutDir() {
    std::wstring d = AppDir() + L"\\layouts";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

// ======================================================================= Datenmodell

enum Kind { K_LOOP = 0, K_CAPTURE = 1, K_GEN = 2, K_OUT = 3, K_STREAM = 4, K_AIRPLAY = 5, K_REC = 6, K_SUM = 7 };
enum GenType { G_CLICK = 0, G_SINE = 1, G_PINK = 2, G_SIMMIC = 3 };
static const size_t TS_CAP = 16384;
static inline bool IsSink(int k) { return k == K_OUT || k == K_STREAM || k == K_REC; }
static inline bool IsOutSink(int k) { return k == K_OUT || k == K_STREAM; }   // Ausgaenge mit Latenz (Autopilot, Einmessung); die Aufnahme-Box zaehlt nicht

struct DevInfo {
    std::wstring id, name;
};

// Aufnahme-Box: Senke, die das gemischte Signal als WAV-Datei aufzeichnet (Einstellungen + Rekorder)
struct RecCtx {
    wavrec::Recorder rec;
    int bits = 24;                 // WAV: 16, 24 oder 32 (Float); FLAC: 16 oder 24
    bool flac = false;             // true = FLAC (verlustfrei) statt WAV
    int rate = 48000;              // 44100, 48000, 88200, 96000
    std::wstring dir;              // Zielordner
    std::wstring err;              // letzte Fehlermeldung (Anzeige)
    double t0 = 0;
    uint64_t framesOut = 0;
};
static std::wstring RecDefaultDir() {
    const wchar_t* up = _wgetenv(L"USERPROFILE");
    return std::wstring(up && *up ? up : L"C:") + L"\\Music\\BensAudioRouter";
}

struct Node {
    int id = 0;
    int kind = K_LOOP;
    std::wstring devId, name;
    float x = 40, y = 40;
    int genType = G_CLICK;

    std::atomic<float> gainLin{1.0f};
    float gainDb = 0.0f;
    std::atomic<bool> mute{false};
    std::atomic<double> delayMs{0.0};
    std::atomic<double> trimMs{0.0};
    std::atomic<double> extMs{0.0};

    std::atomic<int> state{0};  // 0 aus, 1 ok, 2 Fehler
    wchar_t err[200] = {};
    IAudioClient* client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    IAudioRenderClient* ren = nullptr;
    int rate = 0, ch = 0, bits = 0;
    bool isFloat = true;
    UINT32 bufFrames = 0;
    double retryAt = 0;
    int openGen = 0;
    SrcBuf sb;
    double lastPacket = 0, lastFill = 0;
    double genT0 = 0;
    uint64_t genCount = 0;
    uint32_t rng = 12345;
    float pinkRows[8] = {};
    std::vector<float> acc;
    std::vector<int16_t> pcm;
    std::atomic<double> streamLatMs{0.0};
    std::vector<uint8_t> flacBuf;

    // akustische Einmessung
    std::atomic<double> calibMs{-1.0};     // gemessene Gesamtlatenz Sink -> Mikrofon (ms), -1 = keine
    std::atomic<double> calibMeas{-1.0};   // Positionsmessung zum Zeitpunkt der Einmessung (Drift-Referenz)
    int calibPad = 0, calibBurst = 0, calibFmt = 0;
    wchar_t calibWhen[24] = {};
    std::vector<float> probe;
    std::atomic<int> probeState{0};        // 0 aus, 1 scharf, 2 laeuft, 3 fertig
    size_t probePos = 0;
    std::atomic<double> probeT0{0.0};
    // Zeitstempel fuer Mikrofon-Quellen (Frame-Index -> Uhrzeit)
    std::vector<uint64_t> tsI = std::vector<uint64_t>(TS_CAP);
    std::vector<double> tsT = std::vector<double>(TS_CAP);
    size_t tsN = 0;
    uint64_t tsLast = 0;

    StreamCtx* st = nullptr;
    AirplayCtx* ap = nullptr;
    RecCtx* rc = nullptr;
    bool remote = false;                // Handy-Mikrofon: Quelle ohne Geraet, gespeist ueber das Web-Interface
    int remRate = 48000; uint64_t remNext = 0; double remOff = 1e18, remLast = 0;
    std::atomic<bool> sumMono{false};   // Summen-Modul: Mono-Summe am Ausgang
    std::vector<float> tmp;             // Summen-Modul: Zwischenpuffer

    std::atomic<float> pkL{0.0f}, pkR{0.0f};
    std::atomic<bool> clip{false};
    float dispL = -90, dispR = -90, holdL = -90, holdR = -90;
    double holdTL = 0, holdTR = 0;
    // Pegelverlauf (UI) fuer die zeitversetzten Zonen-Meter
    double histT[512] = {};
    float histL[512] = {}, histR[512] = {};
    size_t histN = 0;
    std::vector<float> mDispL, mDispR;

    bool dead = false;
    std::atomic<bool> closed{false};
    ~Node() { delete st; delete ap; delete rc; }
};

static int g_nextConnId = 1;
struct Conn;
struct SubState { Conn* in; ConnState cs; int gen; };   // Summen-Modul: Zustand je (Eingangskabel, Ausgang)
struct Conn {
    int id = g_nextConnId++;        // stabile Nummer fuer das Web-Interface
    Node* src = nullptr;
    Node* dst = nullptr;
    ConnState cs;
    int gen = -1;
    int srcCh = 0, dstCh = 0;       // nur Kabel in ein Summen-Modul: Kanal der Quelle / Ziel im Modul (siehe route.h)
    std::vector<SubState> sub;      // nur Kabel Summen-Modul -> Ausgang
};

static CRITICAL_SECTION g_cs;
static std::vector<Node*> g_nodes, g_graveyard;
static std::vector<Conn*> g_conns;
static std::atomic<bool> g_running{false};
static HANDLE g_thread = nullptr;
static std::atomic<int> g_pollMs{2}, g_padMs{20}, g_baseMs{30}, g_drvMs{100};
static std::atomic<uint32_t> g_underruns{0};
static std::atomic<float> g_loopUs{0};
static int g_nextId = 1;
static bool g_autoSync = true;   // Autopilot standardmaessig an
static IMMDeviceEnumerator* g_enumA = nullptr;
static std::atomic<bool> g_calibActive{false}, g_calibAbort{false}, g_calibRunning{false};
static std::atomic<Node*> g_calibSink{nullptr};
static wchar_t g_calibMsg[260] = L"";
static std::atomic<double> g_simArrive{0.0};   // nur Selbsttest: simuliertes Mikrofon
static double g_simDelayMs = -1.0;
static std::vector<float> g_simProbe;
static std::vector<DevInfo> g_renderDevs, g_captureDevs;
static bool g_testFake = false;  // nur Selbsttest: simulierte Geraete
static HWND g_hwnd = nullptr;
static const double MAX_DELAY_MS = 10000.0;

// ======================================================================= Audio-Engine

static void SetErr(Node* n, const std::wstring& m) {
    wcsncpy(n->err, m.c_str(), 199);
    n->err[199] = 0;
}

static void CloseNode(Node* n) {
    if (n->kind == K_STREAM && n->st) StreamStop(n->st);
    if (n->kind == K_AIRPLAY && n->ap) ApStop(n->ap);
    if (n->kind == K_REC && n->rc) n->rc->rec.stop();
    if (n->client) n->client->Stop();
    if (n->cap) { n->cap->Release(); n->cap = nullptr; }
    if (n->ren) { n->ren->Release(); n->ren = nullptr; }
    if (n->client) { n->client->Release(); n->client = nullptr; }
    n->state = 0;
}

static float ReadSample(const BYTE* base, int bits, bool isFloat, size_t idx) {
    if (isFloat) {
        if (bits == 32) return ((const float*)base)[idx];
        if (bits == 64) return (float)((const double*)base)[idx];
        return 0.0f;
    }
    switch (bits) {
        case 16: return ((const int16_t*)base)[idx] / 32768.0f;
        case 24: {
            const BYTE* p = base + idx * 3;
            uint32_t u = ((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24);
            return (int32_t)u / 2147483648.0f;
        }
        case 32: return ((const int32_t*)base)[idx] / 2147483648.0f;
    }
    return 0.0f;
}

static const double RING_SECONDS = 14.0;

static bool OpenSource(Node* n) {
    if (n->remote) { n->rate = n->remRate; n->ch = 1; n->sb.init(n->rate, RING_SECONDS); n->tsN = 0; n->remNext = 0; n->remOff = 1e18; n->state = 1; n->openGen++; Log("Handy-Mikrofon bereit (%d Hz)", n->rate); return true; }
    if (n->kind == K_AIRPLAY) {
        std::wstring err;
        if (!n->ap || !ApStart(n->ap, err)) { SetErr(n, err.empty() ? L"AirPlay-Start fehlgeschlagen" : err); return false; }
        n->sb.init(44100, RING_SECONDS);
        n->rate = 44100;
        n->ch = 2;
        n->lastFill = NowSec();
        n->state = 1;
        n->openGen++;
        return true;
    }
    if (n->kind == K_GEN) {
        n->sb.init(48000, RING_SECONDS);
        n->rate = 48000;
        n->ch = 2;
        n->genT0 = NowSec();
        n->genCount = 0;
        n->state = 1;
        n->openGen++;
        return true;
    }
    Com<IMMDevice> dev;
    HRESULT hr = g_enumA->GetDevice(n->devId.c_str(), &dev);
    if (FAILED(hr)) { SetErr(n, L"Gerät nicht gefunden"); return false; }
    Com<IAudioClient> cl;
    hr = dev->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&cl);
    if (FAILED(hr)) { SetErr(n, HrMsg(L"Gerät nicht zu öffnen", hr)); return false; }
    WAVEFORMATEX* fmt = nullptr;
    hr = cl->GetMixFormat(&fmt);
    if (FAILED(hr) || !fmt) { SetErr(n, HrMsg(L"Format nicht lesbar", hr)); return false; }
    n->ch = fmt->nChannels;
    n->rate = (int)fmt->nSamplesPerSec;
    n->bits = fmt->wBitsPerSample;
    n->isFloat = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        n->isFloat = (((const WAVEFORMATEXTENSIBLE*)fmt)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
    DWORD flags = (n->kind == K_LOOP) ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    hr = cl->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, fmt, nullptr);
    CoTaskMemFree(fmt);
    if (FAILED(hr)) { SetErr(n, HrMsg(L"Quelle nicht initialisierbar", hr)); return false; }
    Com<IAudioCaptureClient> cap;
    hr = cl->GetService(IID_IAudioCaptureClient, (void**)&cap);
    if (FAILED(hr)) { SetErr(n, HrMsg(L"Capture-Dienst fehlt", hr)); return false; }
    n->sb.init(n->rate, RING_SECONDS);
    hr = cl->Start();
    if (FAILED(hr)) { SetErr(n, HrMsg(L"Start fehlgeschlagen", hr)); return false; }
    n->client = cl.p; cl.p = nullptr;
    n->cap = cap.p; cap.p = nullptr;
    n->lastPacket = n->lastFill = NowSec();
    n->state = 1;
    n->openGen++;
    Log("Quelle geöffnet: %s (%d Hz, %d Kanäle, %d Bit%s)", U8(n->name).c_str(), n->rate, n->ch, n->bits, n->isFloat ? " float" : "");
    return true;
}

static bool OpenOutput(Node* n) {
    Com<IMMDevice> dev;
    HRESULT hr = g_enumA->GetDevice(n->devId.c_str(), &dev);
    if (FAILED(hr)) { SetErr(n, L"Gerät nicht gefunden"); return false; }
    Com<IAudioClient> cl;
    hr = dev->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&cl);
    if (FAILED(hr)) { SetErr(n, HrMsg(L"Gerät nicht zu öffnen", hr)); return false; }
    WAVEFORMATEX* mf = nullptr;
    hr = cl->GetMixFormat(&mf);
    if (FAILED(hr) || !mf) { SetErr(n, HrMsg(L"Format nicht lesbar", hr)); return false; }
    int rate = (int)mf->nSamplesPerSec;
    CoTaskMemFree(mf);
    WAVEFORMATEX rf = {};
    rf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    rf.nChannels = 2;
    rf.nSamplesPerSec = rate;
    rf.wBitsPerSample = 32;
    rf.nBlockAlign = 8;
    rf.nAvgBytesPerSec = rate * 8;
    REFERENCE_TIME dur = (REFERENCE_TIME)g_drvMs.load() * 10000;
    hr = cl->Initialize(AUDCLNT_SHAREMODE_SHARED,
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, dur, 0, &rf, nullptr);
    if (FAILED(hr)) { SetErr(n, HrMsg(L"Ausgang nicht initialisierbar", hr)); return false; }
    UINT32 bf = 0;
    cl->GetBufferSize(&bf);
    Com<IAudioRenderClient> ren;
    hr = cl->GetService(IID_IAudioRenderClient, (void**)&ren);
    if (FAILED(hr)) { SetErr(n, HrMsg(L"Render-Dienst fehlt", hr)); return false; }
    REFERENCE_TIME lat = 0;
    if (SUCCEEDED(cl->GetStreamLatency(&lat))) n->streamLatMs = (double)lat / 10000.0;
    UINT32 pre = std::min<UINT32>(bf, (UINT32)(rate / 100));
    BYTE* out = nullptr;
    if (SUCCEEDED(ren->GetBuffer(pre, &out))) ren->ReleaseBuffer(pre, AUDCLNT_BUFFERFLAGS_SILENT);
    hr = cl->Start();
    if (FAILED(hr)) { SetErr(n, HrMsg(L"Start fehlgeschlagen", hr)); return false; }
    n->bufFrames = bf;
    n->rate = rate;
    n->client = cl.p; cl.p = nullptr;
    n->ren = ren.p; ren.p = nullptr;
    n->state = 1;
    n->openGen++;
    Log("Ausgang geöffnet: %s (%d Hz, Puffer %u Frames, Stream-Latenz %.1f ms)", U8(n->name).c_str(), rate, bf, n->streamLatMs.load());
    return true;
}

static bool OpenStream(Node* n, double now) {
    std::wstring err;
    if (!n->st || !StreamStart(n->st, now, err)) { SetErr(n, err.empty() ? L"Stream-Start fehlgeschlagen" : err); return false; }
    n->rate = n->st->rate;
    n->state = 1;
    n->openGen++;
    return true;
}

static void FailNode(Node* n, const std::wstring& msg) {
    CloseNode(n);
    SetErr(n, msg);
    n->state = 2;
    n->retryAt = NowSec() + 2.0;
    Log("Node-Fehler %s: %s", U8(n->name).c_str(), U8(msg).c_str());
}

static void UpdatePeak(Node* n, float l, float r) {
    if (l > n->pkL.load()) n->pkL.store(l);
    if (r > n->pkR.load()) n->pkR.store(r);
    if (l >= 0.999f || r >= 0.999f) n->clip = true;
}

static inline float XorRand(uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (float)((int32_t)s) / 2147483648.0f;
}

static void TsAdd(Node* n, uint64_t idx, double t) {
    size_t k = n->tsN % TS_CAP;
    n->tsI[k] = idx;
    n->tsT[k] = t;
    n->tsN++;
}
static bool TsIdxToTime(Node* n, double idx, double& t) {
    size_t cnt = std::min<size_t>(n->tsN, TS_CAP);
    for (size_t j = 0; j < cnt; j++) {
        size_t k = (n->tsN - 1 - j) % TS_CAP;
        if ((double)n->tsI[k] <= idx) { t = n->tsT[k] + (idx - (double)n->tsI[k]) / (double)n->rate; return true; }
    }
    return false;
}
static bool TsTimeToIdx(Node* n, double t, double& idx) {
    size_t cnt = std::min<size_t>(n->tsN, TS_CAP);
    for (size_t j = 0; j < cnt; j++) {
        size_t k = (n->tsN - 1 - j) % TS_CAP;
        if (n->tsT[k] <= t) { idx = (double)n->tsI[k] + (t - n->tsT[k]) * (double)n->rate; return true; }
    }
    return false;
}

static void IngestSimMic(Node* n, double now) {
    uint64_t due = (uint64_t)std::max(0.0, (now - n->genT0) * 48000.0);
    if (n->genCount >= due) return;
    if (g_simProbe.empty()) g_simProbe = calib::Sweep(48000);
    if (n->genCount - n->tsLast >= 480 || n->tsN == 0) { TsAdd(n, n->sb.head.load(), n->genT0 + (double)n->genCount / 48000.0); n->tsLast = n->genCount; }
    double arr = g_simArrive.load();
    while (n->genCount < due) {
        double t = n->genT0 + (double)n->genCount / 48000.0;
        float s = 0.002f * XorRand(n->rng);
        if (arr > 0 && t >= arr && t < arr + 0.6) {
            size_t i = (size_t)((t - arr) * 48000.0 + 0.5);
            if (i < g_simProbe.size()) s += g_simProbe[i];
        }
        n->sb.push(s, s);
        n->genCount++;
    }
}

static void IngestGen(Node* n, double now) {
    if (n->genType == G_SIMMIC) { IngestSimMic(n, now); return; }
    uint64_t due = (uint64_t)std::max(0.0, (now - n->genT0) * 48000.0);
    if (due > n->genCount + 48000) n->genCount = due - 4800;
    float pl = 0;
    while (n->genCount < due) {
        float s = 0;
        uint64_t h = n->genCount;
        uint64_t ph = h % 48000;
        if (n->genType == G_CLICK) {
            if (ph < 96) {
                float env = 0.5f * (1.0f - std::cos(6.2831853f * (float)ph / 96.0f));
                s = 0.5f * env * std::sin(6.2831853f * 2000.0f * (float)ph / 48000.0f);
            }
        } else if (n->genType == G_SINE) {
            s = 0.25f * std::sin(6.2831853f * (float)((h * 1000) % 48000) / 48000.0f);
        } else {
            float w = XorRand(n->rng);  // Paul Kellet "economy" Pink-Filter
            n->pinkRows[0] = 0.99765f * n->pinkRows[0] + w * 0.0990460f;
            n->pinkRows[1] = 0.96300f * n->pinkRows[1] + w * 0.2965164f;
            n->pinkRows[2] = 0.57000f * n->pinkRows[2] + w * 1.0526913f;
            s = 0.12f * (n->pinkRows[0] + n->pinkRows[1] + n->pinkRows[2] + w * 0.1848f);
        }
        n->sb.push(s, s);
        pl = std::max(pl, std::fabs(s));
        n->genCount++;
    }
    UpdatePeak(n, pl, pl);
}

static void IngestCapture(Node* n, double now) {
    bool got = false;
    float pl = 0, pr = 0;
    for (;;) {
        UINT32 pkt = 0;
        HRESULT hr = n->cap->GetNextPacketSize(&pkt);
        if (FAILED(hr)) { FailNode(n, HrMsg(L"Quelle verloren", hr)); return; }
        if (pkt == 0) break;
        BYTE* data = nullptr;
        UINT32 fr = 0;
        DWORD fl = 0;
        UINT64 devPos = 0, qpcPos = 0;
        hr = n->cap->GetBuffer(&data, &fr, &fl, &devPos, &qpcPos);
        if (FAILED(hr)) { FailNode(n, HrMsg(L"Quelle verloren", hr)); return; }
        TsAdd(n, n->sb.head.load(), qpcPos > 0 ? (double)qpcPos / 1e7 : now - (double)fr / (double)n->rate);
        if (fl & AUDCLNT_BUFFERFLAGS_SILENT) {
            for (UINT32 i = 0; i < fr; i++) n->sb.push(0, 0);
        } else {
            for (UINT32 i = 0; i < fr; i++) {
                float l = ReadSample(data, n->bits, n->isFloat, (size_t)i * n->ch);
                float r = (n->ch > 1) ? ReadSample(data, n->bits, n->isFloat, (size_t)i * n->ch + 1) : l;
                n->sb.push(l, r);
                pl = std::max(pl, std::fabs(l));
                pr = std::max(pr, std::fabs(r));
            }
        }
        n->cap->ReleaseBuffer(fr);
        got = true;
    }
    if (got) {
        n->lastPacket = now;
        n->lastFill = now;
    } else if (n->kind == K_LOOP && now - n->lastPacket > 0.03) {
        uint64_t fr = (uint64_t)std::max(0.0, (now - n->lastFill) * n->rate);
        if (fr > 0) TsAdd(n, n->sb.head.load(), n->lastFill);
        for (uint64_t i = 0; i < fr; i++) n->sb.push(0, 0);
        if (fr > 0) n->lastFill = now;
    }
    UpdatePeak(n, pl, pr);
}

// AirPlay: Audio aus der Warteschlange zeitgesteuert (44,1 kHz) in den Ringpuffer; bei Pause Stille nach Uhrzeit
static void IngestAirplay(Node* n, double now) {
    AirplayCtx* ap = n->ap;
    const int R = 44100;
    if (ap->flushReq.exchange(false)) { ap->qr.store(ap->qw.load()); ap->playing = false; n->lastFill = now; }
    uint64_t avail = (ap->qw.load(std::memory_order_acquire) - ap->qr.load()) / 2;
    float db = ap->volDb.load();
    float g = ap->useVol.load() ? (db <= -100.0f ? 0.0f : std::pow(10.0f, std::min(0.0f, db) / 20.0f)) : 1.0f;
    float pl = 0, pr = 0;
    auto pushFrames = [&](uint64_t count) {
        uint64_t r = ap->qr.load();
        for (uint64_t i = 0; i < count; i++) {
            float l = ap->q[(size_t)((r + i * 2) % ap->qcap)] * g, rr = ap->q[(size_t)((r + i * 2 + 1) % ap->qcap)] * g;
            n->sb.push(l, rr);
            pl = std::max(pl, std::fabs(l));
            pr = std::max(pr, std::fabs(rr));
        }
        ap->qr.store(r + count * 2, std::memory_order_release);
    };
    if (!ap->playing) {
        uint64_t fr = (uint64_t)std::max(0.0, (now - n->lastFill) * R);   // nie negativ (Zeit wurde evtl. vor dem Oeffnen erfasst)
        if (fr > 0) { for (uint64_t i = 0; i < fr; i++) n->sb.push(0, 0); n->lastFill = now; }
        if (avail >= (uint64_t)ap->bufferMs.load() * R / 1000 && avail > 0) { ap->playing = true; ap->t0 = now; ap->consumed = 0; }
    } else {
        uint64_t due = (uint64_t)std::max(0.0, (now - ap->t0) * R);
        due = due > ap->consumed ? due - ap->consumed : 0;
        uint64_t take = std::min(due, avail);
        pushFrames(take);
        ap->consumed += take;
        if (due > take && due - take > (uint64_t)R / 10) {   // echte Unterbrechung (>100 ms): neu puffern
            uint64_t miss = due - take;
            for (uint64_t i = 0; i < miss; i++) n->sb.push(0, 0);
            ap->playing = false;
            n->lastFill = now;
        }
    }
    UpdatePeak(n, pl, pr);
}

static double LagFor(Node* src, Node* dst) {
    double v = (double)g_baseMs.load() + src->delayMs.load() + dst->delayMs.load() + dst->trimMs.load();
    return std::max(v, 5.0);
}

// Summen-Modul: wird beim Mischen direkt aufgeloest (kein eigener Puffer, keine zusaetzliche Latenz): die Quellen des Moduls
// laufen mit derselben Lag-Rechnung wie direkte Kabel in den Ausgang; dazwischen Kanal-Matrix (route.h), Gain, Mute, Mono-Summe.
static void MixSum(Conn* oc, Node* sink, int outRate, int frames) {
    Node* sm = oc->src;
    sm->acc.assign((size_t)frames * 2, 0.0f);
    for (Conn* ic : g_conns) {
        if (ic->dst != sm) continue;
        Node* in = ic->src;
        if (in->state.load() != 1) continue;
        SubState* ss = nullptr;
        for (SubState& x : oc->sub) if (x.in == ic) { ss = &x; break; }
        if (!ss) { oc->sub.push_back({ic, ConnState(), -1}); ss = &oc->sub.back(); }
        if (ss->gen != in->openGen) { ss->cs = ConnState(); ss->gen = in->openGen; }
        sm->tmp.assign((size_t)frames * 2, 0.0f);
        ProcessConn(ss->cs, in->sb, outRate, LagFor(in, sink) + sm->delayMs.load(), frames, sm->tmp.data(), in->mute.load() ? 0.0f : in->gainLin.load());
        route::Add(sm->tmp.data(), sm->acc.data(), frames, ic->srcCh, ic->dstCh);
    }
    float pl = 0, pr = 0;
    route::Finish(sm->acc.data(), sink->acc.data(), frames, sm->mute.load() ? 0.0f : sm->gainLin.load(), sm->sumMono.load(), pl, pr);
    UpdatePeak(sm, pl, pr);
}

static void MixInto(Node* n, int outRate, int frames) {
    for (Conn* c : g_conns) {
        if (c->dst != n) continue;
        Node* s = c->src;
        if (s->kind == K_SUM) { if (s->state.load() == 1) MixSum(c, n, outRate, frames); continue; }
        if (s->state.load() != 1) { c->gen = -1; continue; }
        if (c->gen != s->openGen) { c->cs = ConnState(); c->gen = s->openGen; }
        ProcessConn(c->cs, s->sb, outRate, LagFor(s, n), frames, n->acc.data(), s->mute.load() ? 0.0f : s->gainLin.load());
    }
}

// Messsweep-Injektion: liefert true, solange der Sweep in diesen Block gemischt wird
static inline bool ProbeActive(Node* n) {
    if (!g_calibActive.load() || g_calibSink.load() != n) return false;
    int st = n->probeState.load();
    return st == 1 || st == 2;
}
static inline void ProbeStart(Node* n, double tFirstSample) {
    if (n->probeState.load() != 1) return;
    n->probeT0 = tFirstSample;
    n->probePos = 0;
    n->probeState = 2;
    if (g_simDelayMs >= 0) g_simArrive = tFirstSample + g_simDelayMs / 1000.0;
}
static inline float ProbeNext(Node* n) {
    if (n->probePos >= n->probe.size()) { n->probeState = 3; return 0.0f; }
    float v = n->probe[n->probePos++];
    if (n->probePos >= n->probe.size()) n->probeState = 3;
    return v;
}

static void RenderOutput(Node* n) {
    UINT32 pad = 0;
    HRESULT hr = n->client->GetCurrentPadding(&pad);
    if (FAILED(hr)) { FailNode(n, HrMsg(L"Ausgang verloren (abgesteckt?)", hr)); return; }
    UINT32 target = (UINT32)((int64_t)n->rate * g_padMs.load() / 1000);
    UINT32 want = pad < target ? target - pad : 0;
    if (want > n->bufFrames - pad) want = n->bufFrames - pad;
    if (pad == 0) g_underruns++;
    if (want == 0) return;
    n->acc.assign((size_t)want * 2, 0.0f);
    if (!g_calibActive.load()) MixInto(n, n->rate, (int)want);
    const bool probing = ProbeActive(n);
    if (probing) ProbeStart(n, NowSec());
    BYTE* out = nullptr;
    hr = n->ren->GetBuffer(want, &out);
    if (FAILED(hr)) { FailNode(n, HrMsg(L"Ausgang verloren (abgesteckt?)", hr)); return; }
    float* o = (float*)out;
    const float g = n->mute.load() ? 0.0f : n->gainLin.load();
    float pl = 0, pr = 0;
    for (UINT32 i = 0; i < want; i++) {
        float l = n->acc[2 * i] * g, r = n->acc[2 * i + 1] * g;
        if (probing && n->probeState.load() == 2) { float pv = ProbeNext(n); l += pv; r += pv; }
        pl = std::max(pl, std::fabs(l));
        pr = std::max(pr, std::fabs(r));
        o[2 * i] = std::max(-1.0f, std::min(1.0f, l));
        o[2 * i + 1] = std::max(-1.0f, std::min(1.0f, r));
    }
    n->ren->ReleaseBuffer(want, 0);
    UpdatePeak(n, pl, pr);
}

static void ProduceStream(Node* n, double now) {
    StreamCtx* sc = n->st;
    uint64_t due = (uint64_t)std::max(0.0, (now - sc->t0) * sc->rate);
    const bool flacFmt = sc->Flac();
    while (sc->framesOut < due) {
        size_t frames = (size_t)std::min<uint64_t>(due - sc->framesOut, 4096);
        n->acc.assign(frames * 2, 0.0f);
        if (!g_calibActive.load()) MixInto(n, sc->rate, (int)frames);
        n->pcm.resize(frames * 2);
        const float g = n->mute.load() ? 0.0f : n->gainLin.load();
        const bool dith = sc->dither.load();
        const bool probing = ProbeActive(n);
        if (probing) ProbeStart(n, sc->t0 + (double)sc->framesOut / (double)sc->rate);
        float pl = 0, pr = 0;
        for (size_t i = 0; i < frames; i++) {
            float l = n->acc[2 * i] * g, r = n->acc[2 * i + 1] * g;
            if (probing && n->probeState.load() == 2) { float pv = ProbeNext(n); l += pv; r += pv; }
            pl = std::max(pl, std::fabs(l));
            pr = std::max(pr, std::fabs(r));
            if (flacFmt) {
                sc->fL[(size_t)sc->fFill] = sc->dth.to24(l, dith);
                sc->fR[(size_t)sc->fFill] = sc->dth.to24(r, dith);
                if (++sc->fFill == 4096) {
                    n->flacBuf.clear();
                    flac::EncodeFrame(n->flacBuf, sc->fL.data(), sc->fR.data(), 4096, 24, sc->rate, sc->fFrameNo++);
                    StreamPushFlacFrame(sc, n->flacBuf, sc->fSamples);
                    sc->fSamples += 4096;
                    sc->fFill = 0;
                }
            } else {
                n->pcm[2 * i] = sc->dth.to16(l, dith);
                n->pcm[2 * i + 1] = sc->dth.to16(r, dith);
            }
        }
        if (!flacFmt) StreamPush(sc, n->pcm.data(), frames);
        sc->framesOut += frames;
        UpdatePeak(n, pl, pr);
    }
}

// ======================================================================= Aufnahme-Box (WAV)

static const wchar_t* RecFmtName(int bits, bool flac) {
    if (flac) return bits == 16 ? L"FLAC 16 Bit" : L"FLAC 24 Bit";
    return bits == 16 ? L"WAV 16 Bit" : bits == 32 ? L"WAV 32 Bit Float" : L"WAV 24 Bit";
}

static bool OpenSum(Node* n) {   // reines Routing-Modul: kein Geraet, kein Takt
    n->state = 1;
    n->openGen++;
    n->rate = 0;
    Log("Summen-Modul bereit: %s", U8(n->name).c_str());
    return true;
}

static bool OpenRec(Node* n, double now) {
    if (!n->rc) { SetErr(n, L"Aufnahme-Kontext fehlt"); return false; }
    n->rc->t0 = now;
    n->rc->framesOut = 0;
    n->rate = n->rc->rate;
    n->ch = 2;
    n->state = 1;
    n->openGen++;
    Log("Aufnahme-Box bereit: %s (%d Hz)", U8(n->name).c_str(), n->rate);
    return true;
}

// Eigener Takt wie beim Stream-Ausgang: Quellen werden auf die Aufnahme-Samplerate resampelt (Drift-Regelung inklusive).
static void ProduceRec(Node* n, double now) {
    RecCtx* rc = n->rc;
    const int rate = rc->rate;
    uint64_t due = (uint64_t)std::max(0.0, (now - rc->t0) * rate);
    if (due > rc->framesOut + (uint64_t)rate * 2) {   // Engine war blockiert: nicht nachholen, sonst entstuenden Luecken im Pegel
        Log("Aufnahme-Box: Takt neu aufgesetzt (Engine war %.1f s blockiert)", (double)(due - rc->framesOut) / rate);
        rc->framesOut = due;
    }
    while (rc->framesOut < due) {
        size_t frames = (size_t)std::min<uint64_t>(due - rc->framesOut, 4096);
        n->acc.assign(frames * 2, 0.0f);
        if (!g_calibActive.load()) MixInto(n, rate, (int)frames);
        const float g = n->mute.load() ? 0.0f : n->gainLin.load();
        float pl = 0, pr = 0;
        for (size_t i = 0; i < frames * 2; i += 2) {
            float l = n->acc[i] * g, r = n->acc[i + 1] * g;
            pl = std::max(pl, std::fabs(l));
            pr = std::max(pr, std::fabs(r));
            n->acc[i] = l;
            n->acc[i + 1] = r;
        }
        if (rc->rec.active()) rc->rec.push(n->acc.data(), frames);
        rc->framesOut += frames;
        UpdatePeak(n, pl, pr);
    }
}

static bool RecMkDirs(const std::wstring& d) {
    std::wstring p;
    for (size_t i = 0; i < d.size(); i++) {
        p += d[i];
        if ((d[i] == L'\\' || d[i] == L'/') && i > 2) CreateDirectoryW(p.c_str(), nullptr);
    }
    CreateDirectoryW(d.c_str(), nullptr);
    DWORD a = GetFileAttributesW(d.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool RecStart(Node* n) {
    RecCtx* rc = n ? n->rc : nullptr;
    if (!rc) return false;
    if (rc->rec.active()) return true;
    if (!g_running || n->state.load() != 1) { rc->err = L"Erst „Start“ drücken – die Box muss grün leuchten."; return false; }
    std::wstring d = rc->dir.empty() ? RecDefaultDir() : rc->dir;
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    if (!RecMkDirs(d)) { rc->err = L"Zielordner nicht verfügbar: " + d; return false; }
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t nm[96];
    swprintf(nm, 96, L"\\Aufnahme_%04d-%02d-%02d_%02d-%02d-%02d.%ls", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, rc->flac ? L"flac" : L"wav");
    std::string err;
    EnterCriticalSection(&g_cs);   // kein push() des Audio-Threads waehrend des Starts
    bool ok = rc->rec.start(U8(d + nm), rc->rate, rc->bits, rc->flac, err);
    LeaveCriticalSection(&g_cs);
    if (!ok) { rc->err = W8(err); Log("Aufnahme: Start fehlgeschlagen – %s", err.c_str()); return false; }
    rc->err.clear();
    Log("Aufnahme gestartet: %s (%ls, %d Hz)", U8(d + nm).c_str(), RecFmtName(rc->bits, rc->flac), rc->rate);
    return true;
}

static void RecStop(Node* n) {
    if (!n || !n->rc) return;
    RecCtx* rc = n->rc;
    bool was = rc->rec.active();
    EnterCriticalSection(&g_cs);
    rc->rec.stop();
    LeaveCriticalSection(&g_cs);
    if (was) Log("Aufnahme beendet: %.1f s, %.1f MB%s", (double)rc->rec.frames() / std::max(1, rc->rate), (double)rc->rec.bytes() / 1048576.0,
                 rc->rec.overruns() ? " – ACHTUNG: Aussetzer (Platte zu langsam)" : "");
}
static void RecToggle(Node* n) { if (n && n->rc) { if (n->rc->rec.active()) RecStop(n); else RecStart(n); } }

static std::wstring RecStatusLine(Node* n) {
    RecCtx* rc = n->rc;
    if (!rc) return L"";
    if (n->state.load() == 2) return std::wstring(L"! ") + n->err;
    if (!g_running) return L"Engine gestoppt – Start drücken";
    if (rc->rec.failed()) return L"! " + W8(rc->rec.lastError());
    wchar_t b[200];
    if (rc->rec.active()) {
        double s = (double)rc->rec.frames() / std::max(1, rc->rate);
        swprintf(b, 200, L"● %02d:%02d:%02d · %.1f MB", (int)(s / 3600), ((int)s / 60) % 60, (int)s % 60, (double)rc->rec.bytes() / 1048576.0);
        std::wstring t = b;
        if (rc->rec.overruns()) t += L" · ! Aussetzer";
        return t;
    }
    if (!rc->err.empty()) return L"! " + rc->err;
    swprintf(b, 200, L"bereit · %ls · %d Hz", RecFmtName(rc->bits, rc->flac), rc->rate);
    return b;
}

static DWORD WINAPI AudioProc(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&g_enumA);
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    while (g_running) {
        Sleep(g_pollMs.load());
        double t0 = NowSec();
        EnterCriticalSection(&g_cs);
        for (Node* g : g_graveyard)
            if (!g->closed) { CloseNode(g); g->closed = true; }
        for (Node* n : g_nodes) {
            int st = n->state.load();
            if (st == 0 || (st == 2 && t0 >= n->retryAt)) {
                bool ok = n->kind == K_OUT ? OpenOutput(n) : n->kind == K_STREAM ? OpenStream(n, t0) : n->kind == K_REC ? OpenRec(n, t0) : n->kind == K_SUM ? OpenSum(n) : OpenSource(n);
                if (!ok) {
                    n->state = 2;
                    n->retryAt = t0 + 2.0;
                    Log("Öffnen fehlgeschlagen: %s – %s", U8(n->name).c_str(), U8(n->err).c_str());
                }
            }
        }
        for (Node* n : g_nodes) {
            if (IsSink(n->kind) || n->kind == K_SUM || n->remote || n->state.load() != 1) continue;
            if (n->kind == K_GEN) IngestGen(n, t0);
            else if (n->kind == K_AIRPLAY) IngestAirplay(n, t0);
            else IngestCapture(n, t0);
        }
        for (Node* n : g_nodes) {
            if (n->state.load() != 1) continue;
            if (n->kind == K_OUT) RenderOutput(n);
            else if (n->kind == K_STREAM) {
                if (n->st->autoFallback.exchange(false) && n->st->format == 1) {
                    Log("Sonos hat FLAC nicht akzeptiert -> automatische Umstellung auf WAV 16 Bit");
                    n->st->format = 0;
                    CloseNode(n);
                    PostMessageW(g_hwnd, WM_APP + 30, 0, 0);
                    continue;
                }
                ProduceStream(n, t0);
            }
            else if (n->kind == K_REC) ProduceRec(n, t0);
        }
        LeaveCriticalSection(&g_cs);
        float us = (float)((NowSec() - t0) * 1e6);
        g_loopUs.store(g_loopUs.load() * 0.95f + us * 0.05f);
    }
    EnterCriticalSection(&g_cs);
    for (Node* n : g_nodes) CloseNode(n);
    for (Node* g : g_graveyard) { CloseNode(g); g->closed = true; }
    LeaveCriticalSection(&g_cs);
    if (g_enumA) { g_enumA->Release(); g_enumA = nullptr; }
    if (mm) AvRevertMmThreadCharacteristics(mm);
    CoUninitialize();
    return 0;
}

static void StartEngine() {
    if (g_running) return;
    g_running = true;
    g_underruns = 0;
    g_thread = CreateThread(nullptr, 0, AudioProc, nullptr, 0, nullptr);
    if (!g_thread) g_running = false;
    Log("Engine gestartet");
}

static void StopEngine() {
    if (!g_running) return;
    g_running = false;
    if (g_thread) {
        WaitForSingleObject(g_thread, 15000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    Log("Engine gestoppt");
}

// ======================================================================= Graph-Verwaltung

static int ApLatencyFn(void* node);
static Node* AddNode(int kind, const std::wstring& devId, const std::wstring& name, float x, float y) {
    Node* n = new Node();
    n->id = g_nextId++;
    n->kind = kind;
    n->devId = devId;
    n->name = name;
    n->x = x;
    n->y = y;
    if (kind == K_GEN) n->genType = (devId == L"gen:sine") ? G_SINE : (devId == L"gen:pink") ? G_PINK : (devId == L"gen:simmic") ? G_SIMMIC : G_CLICK;
    if (kind == K_STREAM) n->st = new StreamCtx();
    if (kind == K_AIRPLAY) { n->ap = new AirplayCtx(); n->ap->latencyFn = ApLatencyFn; n->ap->latencyArg = n; }
    if (kind == K_REC) { n->rc = new RecCtx(); n->rc->dir = RecDefaultDir(); }
    if (devId == L"remote:phone") n->remote = true;
    EnterCriticalSection(&g_cs);
    g_nodes.push_back(n);
    LeaveCriticalSection(&g_cs);
    return n;
}

static Conn* FindConn(Node* s, Node* d) {
    for (Conn* c : g_conns) if (c->src == s && c->dst == d) return c;
    return nullptr;
}

static void RemoveConn(Conn* c) {
    EnterCriticalSection(&g_cs);
    g_conns.erase(std::remove(g_conns.begin(), g_conns.end(), c), g_conns.end());
    LeaveCriticalSection(&g_cs);
    delete c;
}

static Node* g_hoverNode = nullptr;

static void RemoveNode(Node* n) {
    EnterCriticalSection(&g_cs);
    for (size_t i = 0; i < g_conns.size();) {
        if (g_conns[i]->src == n || g_conns[i]->dst == n) { delete g_conns[i]; g_conns.erase(g_conns.begin() + i); }
        else i++;
    }
    g_nodes.erase(std::remove(g_nodes.begin(), g_nodes.end(), n), g_nodes.end());
    n->dead = true;
    if (g_hoverNode == n) g_hoverNode = nullptr;
    if (g_running) g_graveyard.push_back(n);
    LeaveCriticalSection(&g_cs);
    if (!g_running) delete n;
}

static void SweepGraveyard() {
    EnterCriticalSection(&g_cs);
    for (size_t i = 0; i < g_graveyard.size();) {
        if (g_graveyard[i]->closed) { delete g_graveyard[i]; g_graveyard.erase(g_graveyard.begin() + i); }
        else i++;
    }
    LeaveCriticalSection(&g_cs);
}

struct SyncInfo {
    double total;  // Latenz Sink -> Ohr (ohne Quellen-Lag), ms
    int src;       // 0 Schaetzung, 1 Sonos-Position, 2 Einmessung, 3 manuell
    double err;    // Unsicherheit (ms)
};
static const wchar_t* SyncSrcName(int s) {
    return s == 2 ? L"Einmessung (Mikrofon)" : s == 1 ? L"Sonos-Positionsmessung" : s == 3 ? L"manuell eingetragen" : L"Schätzung";
}

static SyncInfo GetSync(Node* n) {
    double pad = g_padMs.load();
    if (n->kind == K_STREAM && n->st) {
        StreamCtx* s = n->st;
        bool meas = s->autoLat.load() && s->measState.load() == 2;
        double cal = n->calibMs.load();
        if (cal >= 0 && n->calibBurst == s->burstMs && n->calibFmt == s->format) {
            double t = cal;
            double err = 1.0;
            if (meas && n->calibMeas.load() >= 0) {  // Drift seit der Einmessung nachfuehren
                t += std::max(-500.0, std::min(500.0, s->measLatMs.load() - n->calibMeas.load()));
                err = s->measErrMs.load();
            }
            return {t, 2, err};
        }
        if (meas) return {s->measLatMs.load() + n->extMs.load(), 1, s->measErrMs.load()};
        if (n->extMs.load() > 0) return {n->extMs.load(), 3, 0};
        return {std::max(2000.0, (double)s->burstMs) + 500.0, 0, 500.0};
    }
    double cal = n->calibMs.load();
    if (cal >= 0) return {cal + (pad - (double)n->calibPad), 2, 1.0};
    return {n->extMs.load() + pad + n->streamLatMs.load(), n->extMs.load() > 0 ? 3 : 0, 0};
}

static int ApLatencyFn(void* node) {   // Auto-Latenz fuer AirPlay: laengster Weg bis zum Ohr
    Node* src = (Node*)node;
    double best = 0;
    EnterCriticalSection(&g_cs);
    for (Conn* c : g_conns) {
        if (c->src != src) continue;
        Node* d = c->dst;
        std::vector<Node*> targets;   // Summen-Modul: bis zu den Ausgaengen hinter dem Modul verfolgen
        double extra = 0;
        if (d->kind == K_SUM) { extra = d->delayMs.load(); for (Conn* c2 : g_conns) if (c2->src == d && IsOutSink(c2->dst->kind)) targets.push_back(c2->dst); }
        else if (IsOutSink(d->kind)) targets.push_back(d);
        for (Node* t : targets) {
            double v = (double)g_baseMs.load() + src->delayMs.load() + extra + t->delayMs.load() + t->trimMs.load() + GetSync(t).total;
            best = std::max(best, v);
        }
    }
    LeaveCriticalSection(&g_cs);
    return (int)best;
}

static void RecomputeSync() {
    double T = -1;
    std::vector<Node*> outs;
    for (Node* n : g_nodes) {
        if (!IsOutSink(n->kind)) continue;
        bool used = false;
        for (Conn* c : g_conns) if (c->dst == n) used = true;
        if (!used) continue;
        outs.push_back(n);
        T = std::max(T, GetSync(n).total);
    }
    for (Node* n : outs) {
        double d = std::floor((T - GetSync(n).total) * 10.0 + 0.5) / 10.0;
        d = std::min(d, MAX_DELAY_MS);
        if (std::fabs(d - n->delayMs.load()) > 0.05) n->delayMs = d;
    }
}

// ----------------------------------------------------------------------- Einmessung (Worker)

static Node* PickMic() {
    for (Node* n : g_nodes) if (n->remote && n->state.load() == 1 && NowSec() - n->remLast < 2.0) return n;   // Handy-Mikrofon bevorzugen, solange es Daten liefert
    for (Node* n : g_nodes) if (n->kind == K_CAPTURE && !n->remote && n->state.load() == 1) return n;
    for (Node* n : g_nodes) if (n->kind == K_GEN && n->genType == G_SIMMIC && n->state.load() == 1) return n;
    return nullptr;
}
static void CalMsg(const wchar_t* f, ...) {
    va_list ap;
    va_start(ap, f);
    vswprintf(g_calibMsg, 260, f, ap);
    va_end(ap);
}
static const UINT WM_CALIB_DONE = WM_APP + 21;

static void AddSum(std::wstring& sum, const std::wstring& line) {
    sum += line;
    Log("Einmessung: %s", U8(line).c_str());
}

static DWORD WINAPI CalibProc(LPVOID) {
    std::vector<Node*> sinks;
    Node* mic = nullptr;
    Log("Einmessung gestartet");
    EnterCriticalSection(&g_cs);
    mic = PickMic();
    for (Node* n : g_nodes) if (IsOutSink(n->kind) && n->state.load() == 1) sinks.push_back(n);
    LeaveCriticalSection(&g_cs);
    std::wstring summary;
    Log("Einmessung: Mikrofon %s, %d Ausgang/Ausgänge", mic ? U8(mic->name).c_str() : "KEINS", (int)sinks.size());
    if (!mic) {
        CalMsg(L"Einmessung: Kein Mikrofon – bitte „+ Quelle → Eingang: …“ (Mikrofon) hinzufügen und die Engine starten.");
    } else if (sinks.empty()) {
        CalMsg(L"Einmessung: Keine aktiven Ausgänge.");
    } else {
        g_calibAbort = false;
        g_calibActive = true;
        std::vector<float> micProbe = calib::Sweep(mic->rate);
        int done = 0, idx = 0;
        for (Node* s : sinks) {
            idx++;
            if (g_calibAbort || !g_running) break;
            bool isStream = s->kind == K_STREAM;
            if (isStream && s->st->ctl.load() != 2) { AddSum(summary, L"• " + s->name + L": Sonos spielt nicht – übersprungen\n"); continue; }
            CalMsg(L"Einmessung läuft: %ls (%d/%d) – bitte ruhig bleiben …", s->name.c_str(), idx, (int)sinks.size());
            int srate = isStream ? s->st->rate : s->rate;
            s->probeState = 0;
            s->probe = calib::Sweep(srate);
            s->probePos = 0;
            g_calibSink = s;
            s->probeState = 1;
            double t0 = NowSec();
            while (s->probeState.load() < 2 && NowSec() - t0 < 3.0 && !g_calibAbort) Sleep(10);
            if (s->probeState.load() < 2) { g_calibSink = nullptr; s->probeState = 0; AddSum(summary, L"• " + s->name + L": Messsignal konnte nicht gesendet werden\n"); continue; }
            double tSub = s->probeT0.load();
            double win = isStream ? 12.0 : 3.5;
            while (NowSec() < tSub + win + 0.4 && !g_calibAbort) Sleep(20);
            g_calibSink = nullptr;
            if (g_calibAbort) break;
            std::vector<float> seg;
            double idx0 = 0;
            bool ok = false;
            EnterCriticalSection(&g_cs);
            double iA = 0, iB = 0;
            if (TsTimeToIdx(mic, tSub - 0.3, iA) && TsTimeToIdx(mic, tSub + win, iB)) {
                uint64_t head = mic->sb.head.load();
                if (iA >= 0 && iB > iA && (double)head - iA < (double)mic->sb.cap - 100.0 && iB < (double)head) {
                    size_t n = (size_t)(iB - iA);
                    seg.resize(n);
                    uint64_t base = (uint64_t)iA;
                    for (size_t i = 0; i < n; i++) seg[i] = mic->sb.d[((base + i) % mic->sb.cap) * 2];
                    idx0 = (double)base;
                    ok = true;
                }
            }
            LeaveCriticalSection(&g_cs);
            s->probeState = 0;
            if (!ok) { AddSum(summary, L"• " + s->name + L": Mikrofon-Aufnahme nicht verfügbar\n"); continue; }
            calib::Peak pk = calib::Locate(seg, micProbe, mic->rate);
            if (!pk.ok) {
                wchar_t b[200];
                swprintf(b, 200, L"• %ls: Messsignal nicht erkannt (SNR %.1f) – Mikrofon näher an den Lautsprecher, Pegel prüfen\n", s->name.c_str(), pk.snr);
                AddSum(summary, b);
                continue;
            }
            double arrive = 0;
            EnterCriticalSection(&g_cs);
            bool tok = TsIdxToTime(mic, idx0 + pk.idx, arrive);
            LeaveCriticalSection(&g_cs);
            if (!tok) { AddSum(summary, L"• " + s->name + L": Zeitstempel fehlen\n"); continue; }
            double L = (arrive - tSub) * 1000.0;
            s->calibMs = L;
            s->calibPad = g_padMs.load();
            s->calibBurst = isStream ? s->st->burstMs : 0;
            s->calibFmt = isStream ? s->st->format : 0;
            s->calibMeas = (isStream && s->st->measState.load() == 2) ? s->st->measLatMs.load() : -1.0;
            SYSTEMTIME st;
            GetLocalTime(&st);
            swprintf(s->calibWhen, 24, L"%02d.%02d. %02d:%02d", st.wDay, st.wMonth, st.wHour, st.wMinute);
            wchar_t b[200];
            swprintf(b, 200, L"• %ls: %.1f ms (SNR %.0f)\n", s->name.c_str(), L, pk.snr);
            AddSum(summary, b);
            Log("Einmessung %s: %.2f ms (SNR %.1f)", U8(s->name).c_str(), L, pk.snr);
            done++;
        }
        g_calibActive = false;
        g_calibSink = nullptr;
        g_simArrive = 0;
        if (g_calibAbort) CalMsg(L"Einmessung abgebrochen.");
        else CalMsg(L"Einmessung fertig: %d von %d Ausgängen gemessen.", done, (int)sinks.size());
    }
    g_calibActive = false;
    g_calibRunning = false;
    PostMessageW(g_hwnd, WM_CALIB_DONE, 0, 0);
    (void)summary;
    return 0;
}

static void StartCalibration() {
    if (g_calibRunning.exchange(true)) { g_calibAbort = true; return; }
    if (!g_running) { g_calibRunning = false; CalMsg(L"Einmessung: Bitte zuerst die Engine starten."); return; }
    CalMsg(L"Einmessung startet …");
    HANDLE h = CreateThread(nullptr, 0, CalibProc, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else g_calibRunning = false;
}

// ======================================================================= Geraete

static void EnumerateDevices() {
    g_renderDevs.clear();
    g_captureDevs.clear();
    Com<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&en))) return;
    for (int pass = 0; pass < 2; pass++) {
        Com<IMMDeviceCollection> col;
        if (FAILED(en->EnumAudioEndpoints(pass == 0 ? eRender : eCapture, DEVICE_STATE_ACTIVE, &col))) continue;
        UINT cnt = 0;
        col->GetCount(&cnt);
        for (UINT i = 0; i < cnt; i++) {
            Com<IMMDevice> d;
            if (FAILED(col->Item(i, &d))) continue;
            LPWSTR id = nullptr;
            if (FAILED(d->GetId(&id)) || !id) continue;
            std::wstring sid = id;
            CoTaskMemFree(id);
            std::wstring name = sid;
            Com<IPropertyStore> ps;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) name = pv.pwszVal;
                PropVariantClear(&pv);
            }
            (pass == 0 ? g_renderDevs : g_captureDevs).push_back({sid, name});
        }
    }
    wchar_t ev[8] = {};
    if (g_renderDevs.empty() && GetEnvironmentVariableW(L"BAR_TEST_FAKEDEVS", ev, 8)) {
        g_testFake = true;
        g_renderDevs.push_back({L"fake-virtual", L"Hi-Fi Cable Input (VB-Audio Hi-Fi Cable)"});
        g_renderDevs.push_back({L"fake-local", L"Lautsprecher (MiniFuse 1)"});
    }
}

// ----------------------------------------------------------------------- Virtuelles Wiedergabegeraet (Standardgeraet umschalten)
// Undokumentierte, aber weit verbreitete Schnittstelle (SoundSwitch, SoundVolumeView u.a.)
struct IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
static const GUID IID_IPolicyConfig_ = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
static const GUID CLSID_PolicyConfigClient_ = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
static bool IsVirtualName(const std::wstring& name) {
    std::wstring n = Lower(name);
    static const wchar_t* keys[] = {L"cable input", L"hifi cable", L"hi-fi cable", L"vb-audio", L"virtual audio", L"virtual cable",
                                    L"voicemeeter", L"virtual 3", L"virtual mix", L"(virtual"};
    for (const wchar_t* k : keys) if (n.find(k) != std::wstring::npos) return true;
    return false;
}

static std::wstring GetDefaultRenderId() {
    if (g_testFake) return L"fake-local";
    Com<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&en))) return L"";
    Com<IMMDevice> d;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d))) return L"";
    LPWSTR id = nullptr;
    if (FAILED(d->GetId(&id)) || !id) return L"";
    std::wstring r = id;
    CoTaskMemFree(id);
    return r;
}

static HRESULT SetDefaultRender(const std::wstring& id) {
    if (g_testFake) return S_OK;
    IPolicyConfig* pc = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_PolicyConfigClient_, nullptr, CLSCTX_ALL, IID_IPolicyConfig_, (void**)&pc);
    if (FAILED(hr) || !pc) return FAILED(hr) ? hr : E_FAIL;
    HRESULT h1 = pc->SetDefaultEndpoint(id.c_str(), eConsole);
    HRESULT h2 = pc->SetDefaultEndpoint(id.c_str(), eMultimedia);
    pc->SetDefaultEndpoint(id.c_str(), eCommunications);
    pc->Release();
    return FAILED(h1) ? h1 : h2;
}

static std::vector<SonosDev> g_sonos;
static CRITICAL_SECTION g_sonosCs;
static std::atomic<bool> g_discovering{false};
static const UINT WM_SONOS_DONE = WM_APP + 20;

static DWORD WINAPI DiscoverProc(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto v = SsdpDiscover(3000);
    EnterCriticalSection(&g_sonosCs);
    g_sonos = v;
    LeaveCriticalSection(&g_sonosCs);
    Log("Sonos-Suche: %d Gerät(e) gefunden", (int)v.size());
    g_discovering = false;
    PostMessageW(g_hwnd, WM_SONOS_DONE, 0, 0);
    CoUninitialize();
    return 0;
}

static void StartDiscovery() {
    if (g_discovering.exchange(true)) return;
    HANDLE h = CreateThread(nullptr, 0, DiscoverProc, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else g_discovering = false;
}

// ======================================================================= Speichern / Laden (Layouts)

static const int PAD_VALS[] = {10, 20, 30, 50, 100};
static const int BASE_VALS[] = {10, 20, 30, 50, 100, 200, 500};
static const int POLL_VALS[] = {1, 2, 3, 5};
static const int DRV_VALS[] = {20, 50, 100, 200};

static std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> v;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(sep, a);
        if (b == std::wstring::npos) { v.push_back(s.substr(a)); break; }
        v.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return v;
}

static std::wstring GraphPath() { return AppDir() + L"\\graph.txt"; }

static bool SaveGraphTo(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    auto w = [&](const std::wstring& s) {
        std::string u = U8(s);
        fwrite(u.data(), 1, u.size(), f);
        fwrite("\n", 1, 1, f);
    };
    wchar_t b[1200];
    swprintf(b, 1200, L"S\t%d\t%d\t%d\t%d\t%d", g_padMs.load(), g_baseMs.load(), g_pollMs.load(), g_drvMs.load(), g_autoSync ? 1 : 0);
    w(b);
    for (Node* n : g_nodes) {
        swprintf(b, 1200, L"N\t%d\t%d\t%d\t%d\t%.1f\t%d\t%.3f\t%.3f\t%d\t%ls\t%ls", n->id, n->kind, (int)n->x, (int)n->y, n->gainDb,
                 n->mute.load() ? 1 : 0, n->delayMs.load(), n->extMs.load(), n->genType, n->devId.c_str(), n->name.c_str());
        w(b);
        swprintf(b, 1200, L"P\t%d\ttrim\t%.3f", n->id, n->trimMs.load());
        w(b);
        if (n->calibMs.load() >= 0) {
            swprintf(b, 1200, L"P\t%d\tcalib\t%.3f;%d;%d;%d;%.3f;%ls", n->id, n->calibMs.load(), n->calibPad, n->calibBurst, n->calibFmt,
                     n->calibMeas.load(), n->calibWhen);
            w(b);
        }
        if (n->kind == K_AIRPLAY && n->ap) {
            AirplayCtx* a = n->ap;
            w(L"P\t" + std::to_wstring(n->id) + L"\tapname\t" + a->name);
            swprintf(b, 1200, L"P\t%d\tapport\t%d", n->id, a->port); w(b);
            swprintf(b, 1200, L"P\t%d\taplatauto\t%d", n->id, a->latAuto.load() ? 1 : 0); w(b);
            swprintf(b, 1200, L"P\t%d\taplatms\t%d", n->id, a->latManualMs.load()); w(b);
            swprintf(b, 1200, L"P\t%d\tapbuf\t%d", n->id, a->bufferMs.load()); w(b);
            swprintf(b, 1200, L"P\t%d\tapvol\t%d", n->id, a->useVol.load() ? 1 : 0); w(b);
        }
        if (n->kind == K_STREAM && n->st) {
            StreamCtx* s = n->st;
            swprintf(b, 1200, L"P\t%d\tip\t%ls", n->id, W8(s->ip).c_str()); w(b);
            swprintf(b, 1200, L"P\t%d\troom\t%ls", n->id, s->room.c_str()); w(b);
            swprintf(b, 1200, L"P\t%d\tport\t%d", n->id, s->port); w(b);
            swprintf(b, 1200, L"P\t%d\trate\t%d", n->id, s->rate); w(b);
            swprintf(b, 1200, L"P\t%d\tburst\t%d", n->id, s->burstMs); w(b);
            swprintf(b, 1200, L"P\t%d\tscheme\t%d", n->id, s->scheme); w(b);
            swprintf(b, 1200, L"P\t%d\tautolat\t%d", n->id, s->autoLat.load() ? 1 : 0); w(b);
            swprintf(b, 1200, L"P\t%d\tdither\t%d", n->id, s->dither.load() ? 1 : 0); w(b);
            swprintf(b, 1200, L"P\t%d\tvol\t%d", n->id, s->sonosVol.load()); w(b);
            {
                std::wstring gl;
                EnterCriticalSection(&s->cs);
                for (size_t i = 0; i < s->groupIps.size(); i++) gl += (i ? L";" : L"") + W8(s->groupIps[i]) + L"|" + (i < s->groupNames.size() ? CleanText(s->groupNames[i]) : L"");
                LeaveCriticalSection(&s->cs);
                if (!gl.empty()) { w(L"P\t" + std::to_wstring(n->id) + L"\tgroup\t" + gl); }
            }
            swprintf(b, 1200, L"P\t%d\tformat\t%d", n->id, s->format); w(b);
            swprintf(b, 1200, L"P\t%d\tvolctl\t%d", n->id, s->volCtl.load() ? 1 : 0); w(b);
        }
        if (n->kind == K_REC && n->rc) {
            swprintf(b, 1200, L"P\t%d\trecbits\t%d", n->id, n->rc->bits); w(b);
            swprintf(b, 1200, L"P\t%d\trecflac\t%d", n->id, n->rc->flac ? 1 : 0); w(b);
            swprintf(b, 1200, L"P\t%d\trecrate\t%d", n->id, n->rc->rate); w(b);
            w(L"P\t" + std::to_wstring(n->id) + L"\trecdir\t" + n->rc->dir);
        }
        if (n->kind == K_SUM) { swprintf(b, 1200, L"P\t%d\tsummono\t%d", n->id, n->sumMono.load() ? 1 : 0); w(b); }
    }
    for (Conn* c : g_conns) {
        swprintf(b, 1200, L"C\t%d\t%d\t%d\t%d", c->src->id, c->dst->id, c->srcCh, c->dstCh);
        w(b);
    }
    fclose(f);
    return true;
}
static void SaveGraph() { SaveGraphTo(GraphPath()); }

static Node* g_sel = nullptr;
static Conn* g_selConn = nullptr;

static void ClearGraph() {
    g_sel = nullptr;
    g_selConn = nullptr;
    std::vector<Node*> copy = g_nodes;
    for (Node* n : copy) RemoveNode(n);
}

static bool LoadGraphFrom(const std::wstring& path, bool replace) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string data;
    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, r);
    fclose(f);
    if (replace) ClearGraph();
    std::wstring text = W8(data);
    std::vector<Node*> byId;
    std::vector<int> ids;
    auto findNode = [&](int id) -> Node* {
        for (size_t i = 0; i < ids.size(); i++) if (ids[i] == id) return byId[i];
        return nullptr;
    };
    for (const std::wstring& lineRaw : Split(text, L'\n')) {
        std::wstring line = lineRaw;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        auto v = Split(line, L'\t');
        if (v.empty()) continue;
        if (v[0] == L"S" && v.size() >= 6) {
            g_padMs = _wtoi(v[1].c_str());
            g_baseMs = _wtoi(v[2].c_str());
            g_pollMs = _wtoi(v[3].c_str());
            g_drvMs = _wtoi(v[4].c_str());
            g_autoSync = _wtoi(v[5].c_str()) != 0;
        } else if (v[0] == L"N" && v.size() >= 12) {
            Node* n = AddNode(_wtoi(v[2].c_str()), v[10], v[11], (float)_wtoi(v[3].c_str()), (float)_wtoi(v[4].c_str()));
            n->gainDb = std::min(0.0f, (float)_wtof(v[5].c_str()));
            n->gainLin = std::pow(10.0f, n->gainDb / 20.0f);
            n->mute = _wtoi(v[6].c_str()) != 0;
            n->delayMs = _wtof(v[7].c_str());
            n->extMs = _wtof(v[8].c_str());
            n->genType = _wtoi(v[9].c_str());
            byId.push_back(n);
            ids.push_back(_wtoi(v[1].c_str()));
        } else if (v[0] == L"P" && v.size() >= 4) {
            Node* n = findNode(_wtoi(v[1].c_str()));
            if (!n) continue;
            const std::wstring& k = v[2];
            const std::wstring& val = v[3];
            if (k == L"trim") n->trimMs = _wtof(val.c_str());
            else if (k == L"calib") {
                auto f = Split(val, L';');
                if (f.size() >= 5) {
                    n->calibMs = _wtof(f[0].c_str());
                    n->calibPad = _wtoi(f[1].c_str());
                    n->calibBurst = _wtoi(f[2].c_str());
                    n->calibFmt = _wtoi(f[3].c_str());
                    n->calibMeas = _wtof(f[4].c_str());
                    if (f.size() >= 6) wcsncpy(n->calibWhen, f[5].c_str(), 23);
                }
            }
            else if (n->ap) {
                AirplayCtx* a = n->ap;
                if (k == L"apname") a->name = CleanText(val);
                else if (k == L"apport") a->port = std::max(1024, std::min(65535, _wtoi(val.c_str())));
                else if (k == L"aplatauto") a->latAuto = _wtoi(val.c_str()) != 0;
                else if (k == L"aplatms") a->latManualMs = std::max(0, std::min(4000, _wtoi(val.c_str())));
                else if (k == L"apbuf") a->bufferMs = std::max(50, std::min(2000, _wtoi(val.c_str())));
                else if (k == L"apvol") a->useVol = _wtoi(val.c_str()) != 0;
            }
            else if (n->st) {
                StreamCtx* s = n->st;
                if (k == L"ip") s->ip = U8(val);
                else if (k == L"room") s->room = val;
                else if (k == L"port") s->port = std::max(1024, std::min(65535, _wtoi(val.c_str())));
                else if (k == L"rate") s->rate = _wtoi(val.c_str()) == 48000 ? 48000 : 44100;
                else if (k == L"burst") s->burstMs = std::max(200, std::min(8000, _wtoi(val.c_str())));
                else if (k == L"scheme") s->scheme = _wtoi(val.c_str()) ? 1 : 0;
                else if (k == L"autolat") s->autoLat = _wtoi(val.c_str()) != 0;
                else if (k == L"dither") s->dither = _wtoi(val.c_str()) != 0;
                else if (k == L"vol") s->sonosVol = _wtoi(val.c_str());
                else if (k == L"format") s->format = _wtoi(val.c_str()) == 0 ? 0 : 1;
                else if (k == L"group") {
                    for (const std::wstring& e : Split(val, L';')) {
                        auto p = Split(e, L'|');
                        if (!p.empty() && !p[0].empty()) { s->groupIps.push_back(U8(p[0])); s->groupNames.push_back(p.size() > 1 ? p[1] : L""); }
                    }
                }
                else if (k == L"volctl") s->volCtl = _wtoi(val.c_str()) != 0;
            }
            else if (n->rc) {
                RecCtx* r = n->rc;
                int iv = _wtoi(val.c_str());
                if (k == L"recbits") r->bits = (iv == 16 || iv == 32) ? iv : 24;
                else if (k == L"recflac") { r->flac = iv != 0; if (r->flac && r->bits == 32) r->bits = 24; }
                else if (k == L"recrate") r->rate = (iv == 44100 || iv == 88200 || iv == 96000) ? iv : 48000;
                else if (k == L"recdir" && !val.empty()) r->dir = val;
            }
            else if (n->kind == K_SUM) {
                if (k == L"summono") n->sumMono = _wtoi(val.c_str()) != 0;
            }
        } else if (v[0] == L"C" && v.size() >= 3) {
            Node* s = findNode(_wtoi(v[1].c_str()));
            Node* d = findNode(_wtoi(v[2].c_str()));
            if (s && d && !FindConn(s, d)) {
                Conn* c = new Conn();
                c->src = s;
                c->dst = d;
                if (v.size() >= 5) { c->srcCh = std::max(0, std::min(3, _wtoi(v[3].c_str()))); c->dstCh = std::max(0, std::min(3, _wtoi(v[4].c_str()))); }
                EnterCriticalSection(&g_cs);
                g_conns.push_back(c);
                LeaveCriticalSection(&g_cs);
            }
        }
    }
    return true;
}

static int g_themeMode = 0;  // 0 auto, 1 dunkel, 2 hell
static std::wstring SettingsPath() { return AppDir() + L"\\settings.ini"; }

// ======================================================================= UI: IDs, Globals

enum {
    IDC_BTN_SRC = 100, IDC_BTN_DST, IDC_BTN_STREAM, IDC_BTN_START, IDC_BTN_VIRT, IDC_BTN_ABOUT, IDC_BTN_LAYOUT, IDC_BTN_THEME,
    IDC_CMB_PAD, IDC_CMB_BASE, IDC_CMB_POLL, IDC_CMB_DRV, IDC_TG_SYNC, IDC_BTN_SYNCNOW, IDC_BTN_RESCAN, IDC_BTN_REC, IDC_BTN_SUM, IDC_BTN_WEB, IDC_BTN_MORE, IDC_BTN_BG,
    IDC_I_GAIN = 200, IDC_I_GAINVAL, IDC_I_MUTE, IDC_I_DLY_EDIT, IDC_I_DLY_SL, IDC_I_TRIM_EDIT, IDC_I_TRIM_SL, IDC_I_EXT_EDIT,
    IDC_I_GEN, IDC_I_SONOS, IDC_I_SEARCH, IDC_I_IP, IDC_I_PORT, IDC_I_SCHEME, IDC_I_RATE, IDC_I_BURST, IDC_I_BURSTVAL,
    IDC_I_AUTOLAT, IDC_I_DITHER, IDC_I_VOL, IDC_I_VOLVAL, IDC_I_PLAY, IDC_I_STOPSON, IDC_I_INFO, IDC_I_MEAS,
    IDC_I_RECONNECT, IDC_I_REMOVE, IDC_I_FMT, IDC_I_VOLCTL, IDC_I_GRPCMB, IDC_I_GRPADD, IDC_I_GRPCLR,
    IDC_I_APNAME, IDC_I_APPORT, IDC_I_APBUF, IDC_I_APLATAUTO, IDC_I_APLATMS, IDC_I_APVOL,
    IDC_I_RECBTN, IDC_I_RECFMT, IDC_I_RECRATE, IDC_I_RECDIR, IDC_I_RECBROWSE, IDC_I_RECOPEN,
    IDC_I_DLYB0 = 260,  // 6 Buttons
    IDC_I_TRIMB0 = 270,  // 4 Buttons
    IDC_I_SUMSRC0 = 280, IDC_I_SUMDST0 = 290, IDC_I_SUMMONO = 300
};
static const wchar_t* DLY_TXT[6] = {L"−10", L"−1", L"−0,1", L"+0,1", L"+1", L"+10"};
static const double DLY_VAL[6] = {-10, -1, -0.1, 0.1, 1, 10};
static const wchar_t* TRIM_TXT[4] = {L"−1", L"−0,1", L"+0,1", L"+1"};
static const double TRIM_VAL[4] = {-1, -0.1, 0.1, 1};

static const int NODE_W = 270, NODE_H = 124;
static const int MEM_HDR = 16, MEM_ROW = 46, MEM_PAD = 6;
static int ZoneCount(Node* n) {
    if (!n->st || n->st->ip.empty()) return 0;
    return 1 + (int)n->st->groupIps.size();
}
static int NodeH(Node* n) {
    int zc = ZoneCount(n);
    return zc ? NODE_H + MEM_HDR + zc * MEM_ROW + MEM_PAD : NODE_H;
}
static int ZoneVol(StreamCtx* s, int z) {
    if (z == 0) return s->coordVol.load();
    int r = -1;
    EnterCriticalSection(&s->cs);
    if (z - 1 < (int)s->groupVol.size()) r = s->groupVol[z - 1];
    LeaveCriticalSection(&s->cs);
    return r;
}
static void SetZoneWant(StreamCtx* s, int z, int v) {
    v = std::max(0, std::min(100, v));
    if (z == 0) { s->coordWant = v; s->coordVol = v; return; }
    EnterCriticalSection(&s->cs);
    s->EnsureVol();
    if (z - 1 < (int)s->groupWant.size()) { s->groupWant[z - 1] = v; s->groupVol[z - 1] = v; }
    LeaveCriticalSection(&s->cs);
}
static const int TOOLBAR_H = 100, INSP_W = 328, STATUS_H = 26;

static HWND g_canvas, g_insp;
static HWND g_btnVirt, g_btnAbout;
static HWND g_btnRec, g_btnSum, g_btnWeb, g_btnMore, g_btnBg;
static HWND g_btnSrc, g_btnDst, g_btnStream, g_btnStart, g_btnLayout, g_btnTheme, g_btnSyncNow, g_btnRescan, g_tgSync;
static HWND g_cmbPad, g_cmbBase, g_cmbPoll, g_cmbDrv;
static HWND g_lblPad, g_lblBase, g_lblPoll, g_lblDrv;
static bool g_inspBusy = false;
static std::wstring g_msg;
static std::wstring g_stText[3];
static int g_scrollY = 0, g_contentH = 0;

struct NativeCtl {
    HWND h;
    int type;
};
static std::vector<NativeCtl> g_native;

// ======================================================================= UI: Inspector-Aufbau

enum { G_HEAD, G_SYNC, G_LEVEL, G_DELAY, G_TRIM, G_EXT, G_GEN, G_REC, G_SUM, G_AIR, G_SONOS, G_GROUP, G_FORMAT, G_LAT, G_INFO, G_BTN, G_COUNT };
struct Item {
    HWND h;
    int g, x, y, w, hh;
};
static std::vector<Item> g_items;
static bool g_gvis[G_COUNT];
static int g_gh[G_COUNT] = {46, 70, 78, 108, 86, 54, 54, 312, 408, 214, 104, 152, 178, 104, 150, 34};
static HWND iHint;

static HWND AddItem(HWND h, int g, int x, int y, int w, int hh) {
    g_items.push_back({h, g, x, y, w, hh});
    return h;
}
static HWND MkStatic(int g, const wchar_t* t, int x, int y, int w, int h, DWORD style = SS_LEFT, HFONT f = nullptr, bool dim = false) {
    HWND c = CreateWindowExW(0, L"STATIC", t, WS_CHILD | style | SS_NOPREFIX, 0, 0, 10, 10, g_insp, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_fUI), TRUE);
    SetWindowLongPtrW(c, GWLP_USERDATA, dim ? 1 : 0);
    return AddItem(c, g, x, y, w, h);
}
static HWND MkEdit(int g, int id, int x, int y, int w, int h, DWORD style = 0, bool mono = true) {
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | style, 0, 0, 10, 10, g_insp,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(mono ? g_fMono : g_fUI), TRUE);
    g_native.push_back({c, 0});
    return AddItem(c, g, x, y, w, h);
}
static HWND MkCombo(int g, int id, int x, int y, int w, int h = 220) {
    HWND c = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0, 0, 10, 10, g_insp,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_fUI, TRUE);
    g_native.push_back({c, 1});
    return AddItem(c, g, x, y, w, h);
}
static HWND MkBtn(int g, const wchar_t* t, int id, int x, int y, int w, int h, int flags = 0) {
    HWND c = CreateWindowExW(0, L"BARBtn", t, WS_CHILD | WS_TABSTOP, 0, 0, 10, 10, g_insp, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, BTM_SETFLAGS, flags, 0);
    return AddItem(c, g, x, y, w, h);
}
static HWND MkSlider(int g, int id, int x, int y, int w, int h, int mn, int mx) {
    HWND c = CreateWindowExW(0, L"BARSlider", L"", WS_CHILD | WS_TABSTOP, 0, 0, 10, 10, g_insp, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, SLM_SETRANGE, mn, mx);
    return AddItem(c, g, x, y, w, h);
}
static HWND MkToggle(int g, const wchar_t* t, int id, int x, int y, int w, int h) {
    HWND c = CreateWindowExW(0, L"BARToggle", t, WS_CHILD | WS_TABSTOP, 0, 0, 10, 10, g_insp, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    return AddItem(c, g, x, y, w, h);
}

static HWND iSumMono, iSumLbl[6], iSumSrc[6], iSumDst[6];
static HWND iRecBtn, iRecStat, iRecFmt, iRecRate, iRecDir, iRecBrowse, iRecOpen;
static HWND iApName, iApPort, iApBuf, iApLatAuto, iApLatMs, iApVol;
static HWND iSyncBig, iSyncSub, iFmt, iVolCtl, iOverview, iGrpCmb, iGrpAdd, iGrpClr, iGrpList;
static HWND iTitle, iKind, iGainVal, iGain, iMute, iDlyLbl, iDlyEdit, iDlySl, iDlyNote, iTrimEdit, iTrimSl, iExtEdit, iGen, iSonos, iSearch, iIp, iPort,
    iScheme, iRate, iBurst, iBurstVal, iAutoLat, iDither, iVol, iVolVal, iPlay, iStopSon, iInfo, iMeas, iReconnect, iRemove, iDlyB[6], iTrimB[4];

static void BuildInspector() {
    RECT z = {0, 0, 10, 10};
    (void)z;
    iHint = CreateWindowExW(0, L"STATIC", L"Node anklicken, um Gain, Verzögerung und Details einzustellen. Kabel ziehst du per Drag & Drop von Port zu Port.",
                            WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, g_insp, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(iHint, WM_SETFONT, (WPARAM)g_fUI, TRUE);
    SetWindowLongPtrW(iHint, GWLP_USERDATA, 1);

    // HEAD
    iTitle = MkStatic(G_HEAD, L"", 0, 0, 296, 22, SS_LEFT | SS_ENDELLIPSIS, g_fBold);
    iKind = MkStatic(G_HEAD, L"", 0, 24, 296, 16, SS_LEFT | SS_ENDELLIPSIS, g_fSmall, true);
    // SYNC
    iSyncBig = MkStatic(G_SYNC, L"", 0, 0, 296, 22, SS_LEFT, g_fBold);
    iSyncSub = MkStatic(G_SYNC, L"", 0, 24, 296, 44, SS_LEFT, g_fSmall, true);
    // LEVEL
    MkStatic(G_LEVEL, L"Gain", 0, 0, 120, 16, SS_LEFT, g_fUI, true);
    iGainVal = MkStatic(G_LEVEL, L"0.0 dB", 196, 0, 100, 16, SS_RIGHT, g_fMono);
    iGain = MkSlider(G_LEVEL, IDC_I_GAIN, 0, 18, 296, 26, -120, 0);
    SendMessageW(iGain, SLM_SETDEF, 0, 0);   // Doppelklick = 0 dB
    SendMessageW(iGain, SLM_SETPOS, 0, 0);
    iMute = MkToggle(G_LEVEL, L"Stummschalten (Mute)", IDC_I_MUTE, 0, 50, 296, 22);
    // DELAY
    iDlyLbl = MkStatic(G_DELAY, L"Verzögerung (ms)", 0, 0, 296, 16, SS_LEFT, g_fUI, true);
    iDlyEdit = MkEdit(G_DELAY, IDC_I_DLY_EDIT, 0, 20, 92, 26, ES_RIGHT);
    iDlyNote = MkStatic(G_DELAY, L"", 100, 25, 196, 16, SS_LEFT, g_fSmall, true);
    for (int i = 0; i < 6; i++) iDlyB[i] = MkBtn(G_DELAY, DLY_TXT[i], IDC_I_DLYB0 + i, i * 50, 52, 46, 26);
    iDlySl = MkSlider(G_DELAY, IDC_I_DLY_SL, 0, 82, 296, 24, 0, 2000);
    // TRIM
    MkStatic(G_TRIM, L"Feinkorrektur (ms) – wirkt zusätzlich", 0, 0, 296, 16, SS_LEFT, g_fUI, true);
    iTrimEdit = MkEdit(G_TRIM, IDC_I_TRIM_EDIT, 0, 20, 80, 26, ES_RIGHT);
    for (int i = 0; i < 4; i++) iTrimB[i] = MkBtn(G_TRIM, TRIM_TXT[i], IDC_I_TRIMB0 + i, 90 + i * 52, 20, 48, 26);
    iTrimSl = MkSlider(G_TRIM, IDC_I_TRIM_SL, 0, 54, 296, 24, -500, 500);
    // EXT
    MkStatic(G_EXT, L"Nachgelagerte Latenz (ms), z. B. Funkstrecke", 0, 0, 296, 16, SS_LEFT, g_fUI, true);
    iExtEdit = MkEdit(G_EXT, IDC_I_EXT_EDIT, 0, 22, 92, 26, ES_RIGHT);
    // GEN
    MkStatic(G_GEN, L"Testsignal", 0, 0, 296, 16, SS_LEFT, g_fUI, true);
    iGen = MkCombo(G_GEN, IDC_I_GEN, 0, 22, 220);
    SendMessageW(iGen, CB_ADDSTRING, 0, (LPARAM)L"Klick (1 Hz)");
    SendMessageW(iGen, CB_ADDSTRING, 0, (LPARAM)L"Sinus 1 kHz");
    SendMessageW(iGen, CB_ADDSTRING, 0, (LPARAM)L"Rosa Rauschen");
    // REC (Aufnahme-Box)
    iRecBtn = MkBtn(G_REC, L"●  Aufnahme starten", IDC_I_RECBTN, 0, 0, 296, 36, BTN_ACCENT);
    iRecStat = MkStatic(G_REC, L"", 0, 42, 296, 40, SS_LEFT, g_fSmall, true);
    MkStatic(G_REC, L"Format (immer genau eine Datei)", 0, 90, 296, 14, SS_LEFT, g_fSmall, true);
    iRecFmt = MkCombo(G_REC, IDC_I_RECFMT, 0, 106, 296);
    SendMessageW(iRecFmt, CB_ADDSTRING, 0, (LPARAM)L"WAV 24 Bit (empfohlen)");
    SendMessageW(iRecFmt, CB_ADDSTRING, 0, (LPARAM)L"FLAC 24 Bit (verlustfrei, klein)");
    SendMessageW(iRecFmt, CB_ADDSTRING, 0, (LPARAM)L"FLAC 16 Bit (verlustfrei, klein)");
    SendMessageW(iRecFmt, CB_ADDSTRING, 0, (LPARAM)L"WAV 16 Bit (mit Dither)");
    SendMessageW(iRecFmt, CB_ADDSTRING, 0, (LPARAM)L"WAV 32 Bit Float");
    MkStatic(G_REC, L"Samplerate", 0, 142, 150, 14, SS_LEFT, g_fSmall, true);
    iRecRate = MkCombo(G_REC, IDC_I_RECRATE, 0, 158, 150);
    SendMessageW(iRecRate, CB_ADDSTRING, 0, (LPARAM)L"44100 Hz");
    SendMessageW(iRecRate, CB_ADDSTRING, 0, (LPARAM)L"48000 Hz");
    SendMessageW(iRecRate, CB_ADDSTRING, 0, (LPARAM)L"88200 Hz");
    SendMessageW(iRecRate, CB_ADDSTRING, 0, (LPARAM)L"96000 Hz");
    MkStatic(G_REC, L"Zielordner", 0, 194, 200, 14, SS_LEFT, g_fSmall, true);
    iRecDir = MkEdit(G_REC, IDC_I_RECDIR, 0, 210, 296, 26, 0, false);
    iRecBrowse = MkBtn(G_REC, L"Ordner wählen…", IDC_I_RECBROWSE, 0, 244, 144, 28);
    iRecOpen = MkBtn(G_REC, L"Ordner öffnen", IDC_I_RECOPEN, 152, 244, 144, 28);
    MkStatic(G_REC, L"Dateiname: Aufnahme_Datum_Uhrzeit.wav/.flac · nach Stopp liegt genau eine fertige Datei vor", 0, 278, 296, 28, SS_LEFT, g_fSmall, true);
    // SUM (Summen-Modul)
    iSumMono = MkToggle(G_SUM, L"Mono-Summe am Ausgang (L+R auf beide Kanäle)", IDC_I_SUMMONO, 0, 0, 296, 22);
    MkStatic(G_SUM, L"Quellen per Kabel in den linken Port ziehen. Je Kabel: welcher Kanal der Quelle und wohin im Modul.", 0, 28, 296, 32, SS_LEFT, g_fSmall, true);
    for (int i = 0; i < 6; i++) {
        int y = 68 + i * 56;
        iSumLbl[i] = MkStatic(G_SUM, L"", 0, y, 296, 16, SS_LEFT | SS_ENDELLIPSIS, g_fUI, true);
        iSumSrc[i] = MkCombo(G_SUM, IDC_I_SUMSRC0 + i, 0, y + 18, 140);
        iSumDst[i] = MkCombo(G_SUM, IDC_I_SUMDST0 + i, 150, y + 18, 146);
        for (const wchar_t* t : {L"Stereo (L+R)", L"nur Links", L"nur Rechts", L"Mono-Summe"}) SendMessageW(iSumSrc[i], CB_ADDSTRING, 0, (LPARAM)t);
        for (const wchar_t* t : {L"→ Stereo", L"→ Links", L"→ Rechts", L"→ Mitte"}) SendMessageW(iSumDst[i], CB_ADDSTRING, 0, (LPARAM)t);
    }
    // AIRPLAY
    MkStatic(G_AIR, L"AirPlay-Name (so erscheint der Router auf iPhone/Mac)", 0, 0, 296, 16, SS_LEFT, g_fUI, true);
    iApName = MkEdit(G_AIR, IDC_I_APNAME, 0, 20, 296, 26, 0, false);
    MkStatic(G_AIR, L"Port", 0, 54, 100, 14, SS_LEFT, g_fSmall, true);
    MkStatic(G_AIR, L"Empfangspuffer (ms)", 120, 54, 176, 14, SS_LEFT, g_fSmall, true);
    iApPort = MkEdit(G_AIR, IDC_I_APPORT, 0, 70, 100, 26, ES_NUMBER | ES_RIGHT);
    iApBuf = MkEdit(G_AIR, IDC_I_APBUF, 120, 70, 100, 26, ES_NUMBER | ES_RIGHT);
    iApLatAuto = MkToggle(G_AIR, L"Latenz automatisch (Autopilot)", IDC_I_APLATAUTO, 0, 106, 296, 22);
    MkStatic(G_AIR, L"Gemeldete Latenz manuell (ms)", 0, 134, 220, 14, SS_LEFT, g_fSmall, true);
    iApLatMs = MkEdit(G_AIR, IDC_I_APLATMS, 0, 150, 100, 26, ES_NUMBER | ES_RIGHT);
    iApVol = MkToggle(G_AIR, L"Lautstärke des iPhones übernehmen", IDC_I_APVOL, 0, 186, 296, 22);
    // SONOS
    MkStatic(G_SONOS, L"Sonos-Zone (UPnP)", 0, 0, 296, 16, SS_LEFT, g_fUI, true);
    iSonos = MkCombo(G_SONOS, IDC_I_SONOS, 0, 22, 206);
    iSearch = MkBtn(G_SONOS, L"Suchen", IDC_I_SEARCH, 214, 22, 82, 26);
    MkStatic(G_SONOS, L"IP-Adresse", 0, 58, 150, 14, SS_LEFT, g_fSmall, true);
    MkStatic(G_SONOS, L"Stream-Port", 160, 58, 100, 14, SS_LEFT, g_fSmall, true);
    iIp = MkEdit(G_SONOS, IDC_I_IP, 0, 74, 150, 26);
    iPort = MkEdit(G_SONOS, IDC_I_PORT, 160, 74, 80, 26, ES_NUMBER | ES_RIGHT);
    // GROUP
    MkStatic(G_GROUP, L"Sonos-Gruppe: weitere Zonen synchron (Sonos-Clock)", 0, 0, 296, 16, SS_LEFT, g_fUI, true);
    iGrpCmb = MkCombo(G_GROUP, IDC_I_GRPCMB, 0, 22, 206);
    iGrpAdd = MkBtn(G_GROUP, L"+ Zone", IDC_I_GRPADD, 214, 22, 82, 26);
    iGrpList = MkStatic(G_GROUP, L"", 0, 56, 296, 62, SS_LEFT, g_fMono);
    iGrpClr = MkBtn(G_GROUP, L"Gruppe leeren", IDC_I_GRPCLR, 0, 122, 144, 26);
    // FORMAT
    MkStatic(G_FORMAT, L"URI-Schema", 0, 0, 180, 14, SS_LEFT, g_fSmall, true);
    MkStatic(G_FORMAT, L"Samplerate", 190, 0, 106, 14, SS_LEFT, g_fSmall, true);
    iScheme = MkCombo(G_FORMAT, IDC_I_SCHEME, 0, 16, 180);
    SendMessageW(iScheme, CB_ADDSTRING, 0, (LPARAM)L"x-rincon-mp3radio://");
    SendMessageW(iScheme, CB_ADDSTRING, 0, (LPARAM)L"http://");
    iRate = MkCombo(G_FORMAT, IDC_I_RATE, 190, 16, 106);
    SendMessageW(iRate, CB_ADDSTRING, 0, (LPARAM)L"44100 Hz");
    SendMessageW(iRate, CB_ADDSTRING, 0, (LPARAM)L"48000 Hz");
    MkStatic(G_FORMAT, L"Audioformat", 0, 50, 200, 14, SS_LEFT, g_fSmall, true);
    iFmt = MkCombo(G_FORMAT, IDC_I_FMT, 0, 66, 296);
    SendMessageW(iFmt, CB_ADDSTRING, 0, (LPARAM)L"FLAC 24 Bit (verlustfrei, empfohlen)");
    SendMessageW(iFmt, CB_ADDSTRING, 0, (LPARAM)L"WAV 16 Bit (maximal kompatibel)");
    MkStatic(G_FORMAT, L"Puffer-Reserve (Vorlauf)", 0, 100, 200, 16, SS_LEFT, g_fUI, true);
    iBurstVal = MkStatic(G_FORMAT, L"1500 ms", 196, 100, 100, 16, SS_RIGHT, g_fMono);
    iBurst = MkSlider(G_FORMAT, IDC_I_BURST, 0, 118, 296, 24, 2, 80);
    iDither = MkToggle(G_FORMAT, L"TPDF-Dither (bei der Bit-Reduktion)", IDC_I_DITHER, 0, 148, 296, 22);
    // LAT
    iAutoLat = MkToggle(G_LAT, L"Sonos-Positionsabfrage (Drift-Tracking)", IDC_I_AUTOLAT, 0, 0, 296, 22);
    iMeas = MkStatic(G_LAT, L"", 0, 26, 296, 34, SS_LEFT, g_fMono);
    iPlay = MkBtn(G_LAT, L"Wiedergabe (neu) starten", IDC_I_PLAY, 0, 64, 184, 30, BTN_ACCENT);
    iStopSon = MkBtn(G_LAT, L"Stopp", IDC_I_STOPSON, 192, 64, 104, 30);
    // INFO
    iInfo = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0, 0, 10, 10, g_insp,
                            (HMENU)(INT_PTR)IDC_I_INFO, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(iInfo, WM_SETFONT, (WPARAM)g_fMono, TRUE);
    g_native.push_back({iInfo, 2});
    AddItem(iInfo, G_INFO, 0, 0, 296, 146);
    iOverview = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0, 0, 10, 10, g_insp,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(iOverview, WM_SETFONT, (WPARAM)g_fMono, TRUE);
    g_native.push_back({iOverview, 2});
    // BTN
    iReconnect = MkBtn(G_BTN, L"Neu verbinden", IDC_I_RECONNECT, 0, 0, 144, 30);
    iRemove = MkBtn(G_BTN, L"Entfernen", IDC_I_REMOVE, 152, 0, 144, 30, BTN_DANGER);
}

static void LayoutInspector() {
    RECT rc;
    GetClientRect(g_insp, &rc);
    if (iOverview) SetWindowPos(iOverview, nullptr, S(12), S(12), S(INSP_W - 40), std::max(100, (int)rc.bottom - S(24)), SWP_NOZORDER | SWP_NOACTIVATE);
    int y = 12;
    for (int g = 0; g < G_COUNT; g++) {
        if (!g_gvis[g]) continue;
        for (Item& it : g_items) {
            if (it.g != g) continue;
            SetWindowPos(it.h, nullptr, S(12 + it.x), S(y + it.y) - g_scrollY, S(it.w), S(it.hh), SWP_NOZORDER | SWP_NOACTIVATE);
        }
        y += g_gh[g] + 12;
    }
    g_contentH = S(y);
    int maxScroll = std::max(0, g_contentH - (int)rc.bottom);
    if (g_scrollY > maxScroll) {
        g_scrollY = maxScroll;
        LayoutInspector();
    }
}

static void ShowGroups(bool hasNode, int kind) {
    for (int i = 0; i < G_COUNT; i++) g_gvis[i] = false;
    if (hasNode) {
        g_gvis[G_HEAD] = g_gvis[G_LEVEL] = g_gvis[G_INFO] = g_gvis[G_BTN] = true;
        if (kind == K_REC) g_gvis[G_REC] = true;
        else if (kind == K_SUM) { g_gvis[G_SUM] = true; g_gvis[G_DELAY] = true; }
        else if (IsSink(kind)) {
            g_gvis[G_SYNC] = g_gvis[G_TRIM] = true;
            if (!g_autoSync) g_gvis[G_DELAY] = g_gvis[G_EXT] = true;
        } else g_gvis[G_DELAY] = true;
        if (kind == K_GEN) g_gvis[G_GEN] = true;
        if (kind == K_AIRPLAY) g_gvis[G_AIR] = true;
        if (kind == K_STREAM) g_gvis[G_SONOS] = g_gvis[G_GROUP] = g_gvis[G_FORMAT] = g_gvis[G_LAT] = true;
    }
    for (Item& it : g_items) ShowWindow(it.h, g_gvis[it.g] ? SW_SHOW : SW_HIDE);
    ShowWindow(iHint, SW_HIDE);
    ShowWindow(iOverview, hasNode ? SW_HIDE : SW_SHOW);
    RECT rc;
    GetClientRect(g_insp, &rc);
    SetWindowPos(iOverview, nullptr, S(12), S(12), S(INSP_W - 40), std::max(100, (int)rc.bottom - S(24)), SWP_NOZORDER);
}

static std::wstring FmtD(double v, int dec = 1) {
    wchar_t b[64];
    swprintf(b, 64, L"%.*f", dec, v);
    return b;
}
static double ParseD(HWND e) {
    wchar_t b[64] = {};
    GetWindowTextW(e, b, 64);
    for (wchar_t* p = b; *p; p++) if (*p == L',') *p = L'.';
    return _wtof(b);
}
static std::string GetTextU8(HWND e) {
    wchar_t b[128] = {};
    GetWindowTextW(e, b, 128);
    return U8(b);
}

static void FillSonosCombo() {
    SendMessageW(iSonos, CB_RESETCONTENT, 0, 0);
    EnterCriticalSection(&g_sonosCs);
    for (auto& d : g_sonos) SendMessageW(iSonos, CB_ADDSTRING, 0, (LPARAM)(d.name + L"  (" + W8(d.ip) + L")").c_str());
    LeaveCriticalSection(&g_sonosCs);
    if (g_sel && g_sel->st) {
        EnterCriticalSection(&g_sonosCs);
        for (size_t i = 0; i < g_sonos.size(); i++)
            if (g_sonos[i].ip == g_sel->st->ip) SendMessageW(iSonos, CB_SETCURSEL, (WPARAM)i, 0);
        LeaveCriticalSection(&g_sonosCs);
    }
}

static const wchar_t* SyncShort(int s) { return s == 2 ? L"Einmessung" : s == 1 ? L"Position" : s == 3 ? L"manuell" : L"Schätzung"; }

static void SetTextIfChanged(HWND h, const std::wstring& t) {
    wchar_t cur[1200] = {};
    GetWindowTextW(h, cur, 1200);
    if (t != cur) SetWindowTextW(h, t.c_str());
}

static void UpdateSyncCard() {
    Node* n = g_sel;
    if (!n || !IsOutSink(n->kind)) return;
    SyncInfo si = GetSync(n);
    wchar_t b[400];
    swprintf(b, 400, L"Latenz Ausgang → Ohr: %.0f ms", si.total);
    SetTextIfChanged(iSyncBig, b);
    std::wstring sub = std::wstring(L"Quelle: ") + SyncSrcName(si.src);
    if (si.src == 2 && n->calibWhen[0]) sub += std::wstring(L" vom ") + n->calibWhen;
    if (si.err > 0) { swprintf(b, 400, L" · ±%.0f ms", si.err); sub += b; }
    swprintf(b, 400, L"\r\nAusgleich: %.1f ms %ls", n->delayMs.load() + n->trimMs.load(), g_autoSync ? L"(Autopilot + Feinkorrektur)" : L"(manuell)");
    sub += b;
    if (si.src == 0) sub += L"\r\n→ Toolbar „Einmessen…“ für den echten Wert";
    SetTextIfChanged(iSyncSub, sub);
}

static std::wstring BuildOverview() {
    std::wstring t = L"SYNC-ÜBERSICHT\r\n";
    t += g_autoSync ? L"Autopilot: AN (Verzögerungen automatisch)\r\n\r\n" : L"Autopilot: AUS (Verzögerung manuell)\r\n\r\n";
    wchar_t b[400];
    int cnt = 0, exact = 0;
    for (Node* n : g_nodes) {
        if (!IsOutSink(n->kind)) continue;
        bool used = false;
        for (Conn* c : g_conns) if (c->dst == n) used = true;
        if (!used) continue;
        cnt++;
        SyncInfo si = GetSync(n);
        if (si.src == 2) exact++;
        swprintf(b, 400, L"%ls\r\n  Latenz %.0f ms · %ls\r\n  Ausgleich %.1f ms (Trim %+.1f)\r\n\r\n", n->name.c_str(), si.total, SyncShort(si.src),
                 n->delayMs.load() + n->trimMs.load(), n->trimMs.load());
        t += b;
    }
    // Topologie-Check: langsamer Ausgang (Sonos) ohne lokalen Ausgang -> lokale Wiedergabe laeuft am Router vorbei
    bool hasLocal = false, hasSlow = false;
    for (Node* n : g_nodes) {
        if (!IsOutSink(n->kind)) continue;
        bool used = false;
        for (Conn* c : g_conns) if (c->dst == n) used = true;
        if (!used) continue;
        if (n->kind == K_OUT) hasLocal = true;
        if (GetSync(n).total > 300.0) hasSlow = true;
    }
    if (hasSlow && !hasLocal)
        t += L"ACHTUNG: Kein lokaler Ausgang im Router!\r\n"
             L"Was Windows direkt auf ein Gerät spielt,\r\nläuft am Router vorbei und kann nicht\r\nverzögert werden.\r\n\r\n"
             L"Lösung: Toolbar „Virtuelles Gerät…“ →\r\nAssistent. Er verkabelt alles und stellt\r\nWindows auf das virtuelle Gerät um.\r\n\r\n";
    if (!cnt) t += L"Noch keine verbundenen Ausgänge.\r\n\r\n";
    else if (exact == cnt) t += L"Alle Ausgänge eingemessen.\r\n\r\n";
    else t += L"Tipp: Mikrofon als Quelle hinzufügen,\r\ndann „Einmessen…“ – misst die echte\r\nLatenz bis zum Lautsprecher.\r\n\r\n";
    if (g_calibMsg[0]) t += std::wstring(g_calibMsg) + L"\r\n";
    return t;
}

static void FillGroupCombo() {
    SendMessageW(iGrpCmb, CB_RESETCONTENT, 0, 0);
    EnterCriticalSection(&g_sonosCs);
    for (auto& d : g_sonos) SendMessageW(iGrpCmb, CB_ADDSTRING, 0, (LPARAM)(d.name + L"  (" + W8(d.ip) + L")").c_str());
    LeaveCriticalSection(&g_sonosCs);
}
static void UpdateGroupList() {
    if (!g_sel || !g_sel->st) return;
    StreamCtx* s = g_sel->st;
    std::wstring t;
    EnterCriticalSection(&s->cs);
    for (size_t i = 0; i < s->groupIps.size(); i++) {
        std::wstring nm = i < s->groupNames.size() ? CleanText(s->groupNames[i]) : L"";
        EnterCriticalSection(&g_sonosCs);
        for (auto& d : g_sonos) if (d.ip == s->groupIps[i]) nm = d.name;
        LeaveCriticalSection(&g_sonosCs);
        t += L"• " + (nm.empty() ? W8(s->groupIps[i]) : nm) + L"  (" + W8(s->groupIps[i]) + L")\r\n";
    }
    LeaveCriticalSection(&s->cs);
    if (t.empty()) t = L"Keine weiteren Zonen.\r\nNur diese Zone spielt.";
    else { wchar_t b[80]; swprintf(b, 80, L"→ %d von %d beigetreten", s->groupOk.load(), (int)s->groupIps.size()); t += b; }
    SetTextIfChanged(iGrpList, t);
}

static void RefreshRecCtl(Node* n) {   // Knopf + Status (laufend aktualisiert)
    RecCtx* rc = n->rc;
    if (!rc) return;
    bool on = rc->rec.active();
    static Node* lastN = nullptr;
    static int lastOn = -1;
    if (lastN != n || lastOn != (on ? 1 : 0)) {
        lastN = n; lastOn = on ? 1 : 0;
        SetWindowTextW(iRecBtn, on ? L"■  Aufnahme stoppen" : L"●  Aufnahme starten");
        SendMessageW(iRecBtn, BTM_SETFLAGS, on ? BTN_DANGER : BTN_ACCENT, 0);
        EnableWindow(iRecFmt, !on); EnableWindow(iRecRate, !on); EnableWindow(iRecDir, !on); EnableWindow(iRecBrowse, !on);
    }
    std::wstring t = RecStatusLine(n);
    t += on ? L"\r\n" + W8(rc->rec.file()) : L"\r\nZiel: " + rc->dir;
    SetTextIfChanged(iRecStat, t);
}
static std::vector<Conn*> SumInputs(Node* sm) {
    std::vector<Conn*> v;
    for (Conn* c : g_conns) if (c->dst == sm) v.push_back(c);
    return v;
}
static void FillSumCtl(Node* n) {
    SendMessageW(iSumMono, TGM_SET, n->sumMono.load() ? 1 : 0, 0);
    std::vector<Conn*> in = SumInputs(n);
    for (int i = 0; i < 6; i++) {
        bool on = i < (int)in.size();
        for (HWND h : {iSumLbl[i], iSumSrc[i], iSumDst[i]}) ShowWindow(h, on && g_gvis[G_SUM] ? SW_SHOW : SW_HIDE);
        if (!on) continue;
        SetWindowTextW(iSumLbl[i], (std::to_wstring(i + 1) + L"   " + in[i]->src->name).c_str());
        SendMessageW(iSumSrc[i], CB_SETCURSEL, in[i]->srcCh, 0);
        SendMessageW(iSumDst[i], CB_SETCURSEL, in[i]->dstCh, 0);
    }
}

static void FillRecCtl(Node* n) {
    RecCtx* rc = n->rc;
    if (!rc) return;
    static const int R[4] = {44100, 48000, 88200, 96000};
    int idx = 1;
    for (int i = 0; i < 4; i++) if (R[i] == rc->rate) idx = i;
    SendMessageW(iRecFmt, CB_SETCURSEL, rc->flac ? (rc->bits == 16 ? 2 : 1) : (rc->bits == 16 ? 3 : rc->bits == 32 ? 4 : 0), 0);
    SendMessageW(iRecRate, CB_SETCURSEL, idx, 0);
    SetWindowTextW(iRecDir, rc->dir.c_str());
    RefreshRecCtl(n);
}

static void UpdateInspector() {
    g_inspBusy = true;
    Node* n = g_sel;
    ShowGroups(n != nullptr, n ? n->kind : 0);
    if (n) {
        SetWindowTextW(iTitle, n->name.c_str());
        const wchar_t* kt = n->kind == K_LOOP ? L"Quelle · Wiedergabe-Mitschnitt (Loopback)"
                          : n->kind == K_CAPTURE ? L"Quelle · Eingang"
                          : n->kind == K_GEN ? L"Quelle · Testgenerator"
                          : n->kind == K_AIRPLAY ? L"Quelle · AirPlay-Empfänger (iPhone / Mac)"
                          : n->kind == K_SUM ? L"Summen-Modul · Kanäle zuordnen (Mono → L/R)"
                          : n->kind == K_REC ? L"Senke · Aufnahme-Box (WAV / FLAC-Datei)"
                          : n->kind == K_STREAM ? L"Stream-Ausgang · Sonos (UPnP, FLAC/WAV)" : L"Ausgang · WASAPI";
        SetWindowTextW(iKind, kt);
        SendMessageW(iGain, SLM_SETPOS, (WPARAM)std::lround(n->gainDb * 2.0f), 0);
        SetWindowTextW(iGainVal, (FmtD(n->gainDb, 1) + L" dB").c_str());
        SendMessageW(iMute, TGM_SET, n->mute.load() ? 1 : 0, 0);
        bool lockDelay = false;
        SetWindowTextW(iDlyLbl, IsSink(n->kind) ? L"Ausgangs-Verzögerung (ms)" : L"Quellen-Verzögerung (ms), wirkt auf alle Ausgänge");
        SetWindowTextW(iDlyEdit, FmtD(n->delayMs.load(), 1).c_str());
        SendMessageW(iDlySl, SLM_SETPOS, (WPARAM)std::lround(std::min(2000.0, n->delayMs.load())), 0);
        SetWindowTextW(iDlyNote, lockDelay ? L"automatisch durch Auto-Sync" : L"");
        EnableWindow(iDlyEdit, !lockDelay);
        EnableWindow(iDlySl, !lockDelay);
        for (int i = 0; i < 6; i++) EnableWindow(iDlyB[i], !lockDelay);
        SetWindowTextW(iTrimEdit, FmtD(n->trimMs.load(), 1).c_str());
        SendMessageW(iTrimSl, SLM_SETPOS, (WPARAM)std::lround(n->trimMs.load() * 10.0), 0);
        if (n->kind == K_OUT || n->kind == K_STREAM) SetWindowTextW(iExtEdit, FmtD(n->extMs.load(), 1).c_str());
        if (n->kind == K_GEN) SendMessageW(iGen, CB_SETCURSEL, n->genType, 0);
        if (n->kind == K_AIRPLAY && n->ap) {
            AirplayCtx* a = n->ap;
            SetWindowTextW(iApName, a->name.c_str());
            SetWindowTextW(iApPort, std::to_wstring(a->port).c_str());
            SetWindowTextW(iApBuf, std::to_wstring(a->bufferMs.load()).c_str());
            SendMessageW(iApLatAuto, TGM_SET, a->latAuto.load() ? 1 : 0, 0);
            SetWindowTextW(iApLatMs, std::to_wstring(a->latManualMs.load()).c_str());
            EnableWindow(iApLatMs, !a->latAuto.load());
            SendMessageW(iApVol, TGM_SET, a->useVol.load() ? 1 : 0, 0);
        }
        if (n->kind == K_STREAM && n->st) {
            StreamCtx* s = n->st;
            FillSonosCombo();
            FillGroupCombo();
            SetWindowTextW(iIp, W8(s->ip).c_str());
            SetWindowTextW(iPort, std::to_wstring(s->port).c_str());
            SendMessageW(iScheme, CB_SETCURSEL, s->scheme, 0);
            SendMessageW(iRate, CB_SETCURSEL, s->rate == 48000 ? 1 : 0, 0);
            SendMessageW(iBurst, SLM_SETPOS, s->burstMs / 100, 0);
            SetWindowTextW(iBurstVal, (std::to_wstring(s->burstMs) + L" ms").c_str());
            SendMessageW(iDither, TGM_SET, s->dither.load() ? 1 : 0, 0);
            SendMessageW(iFmt, CB_SETCURSEL, s->format == 1 ? 0 : 1, 0);
            SendMessageW(iVolCtl, TGM_SET, s->volCtl.load() ? 1 : 0, 0);
            EnableWindow(iVol, s->volCtl.load());
            SendMessageW(iAutoLat, TGM_SET, s->autoLat.load() ? 1 : 0, 0);
            int v = s->sonosVol.load();
            SendMessageW(iVol, SLM_SETPOS, v < 0 ? 0 : v, 0);
            SetWindowTextW(iVolVal, !s->volCtl.load() ? L"nicht gesteuert" : v < 0 ? L"unverändert" : (std::to_wstring(v) + L" %").c_str());
        }
    }
    if (n && n->rc) FillRecCtl(n);
    if (n && n->kind == K_SUM) FillSumCtl(n);
    UpdateSyncCard();
    UpdateGroupList();
    LayoutInspector();
    g_inspBusy = false;
}

static std::wstring ApStatusLine(Node* n) {
    AirplayCtx* a = n->ap;
    if (!a) return L"";
    if (n->state.load() == 2) return std::wstring(L"! ") + n->err;
    if (!g_running) return L"AirPlay aus (Engine gestoppt)";
    if (a->state.load() == 2) {
        std::wstring c;
        EnterCriticalSection(&a->cs);
        c = W8(a->clientIp);
        LeaveCriticalSection(&a->cs);
        wchar_t b[120];
        swprintf(b, 120, L"spielt von %ls · %d ms gemeldet", c.c_str(), a->reportedLatMs.load());
        return b;
    }
    if (a->mdnsState.load() == 2) return L"! mDNS (Port 5353) blockiert – iPhone findet den Router evtl. nicht";
    return L"wartet auf iPhone / Mac …";
}

static std::wstring StreamStatusLine(Node* n) {
    StreamCtx* s = n->st;
    if (!s) return L"";
    if (n->state.load() == 2) return std::wstring(L"! ") + n->err;
    if (!g_running) return L"Stream-Server aus (Engine gestoppt)";
    if (s->ip.empty()) return L"Keine Sonos-IP gesetzt";
    std::wstring t;
    switch (s->ctl.load()) {
        case 1: t = L"verbinde …"; break;
        case 2: t = L"spielt"; break;
        case 3: t = s->msg; break;
        case 4: t = L"wartet/neu starten"; break;
        default: t = L"bereit";
    }
    if (s->ctl.load() == 2) {
        SyncInfo si = GetSync(n);
        wchar_t b[96];
        swprintf(b, 96, L" · %.0f ms (%ls)", si.total, SyncShort(si.src));
        t += b;
        if (!s->groupIps.empty()) { swprintf(b, 96, L" · +%d Zone(n)", (int)s->groupIps.size()); t += b; }
    }
    return t;
}

static void RefreshInspectorInfo() {
    Node* n = g_sel;
    if (!n) { SetTextIfChanged(iOverview, BuildOverview()); return; }
    UpdateSyncCard();
    UpdateGroupList();
    if (n->kind == K_REC) { g_inspBusy = true; RefreshRecCtl(n); g_inspBusy = false; }
    if (n->kind == K_SUM) {   // Zeilen neu aufbauen, sobald Kabel hinzukommen/wegfallen
        static Node* lastSel = nullptr;
        static size_t lastCnt = (size_t)-1;
        size_t cnt = SumInputs(n).size();
        if (lastSel != n || lastCnt != cnt) { lastSel = n; lastCnt = cnt; g_inspBusy = true; FillSumCtl(n); g_inspBusy = false; }
    }
    std::wstring t;
    wchar_t b[500];
    int st = n->state.load();
    if (!g_running) t += L"Status: Engine gestoppt\r\n";
    else if (st == 1) t += L"Status: aktiv\r\n";
    else if (st == 2) t += std::wstring(L"Status: FEHLER – ") + n->err + L"\r\n";
    else t += L"Status: öffne …\r\n";
    if (n->kind == K_OUT) {
        swprintf(b, 500, L"Samplerate: %d Hz\r\nStream-Latenz (WASAPI): %.1f ms\r\nAusgabe-Puffer: %d ms\r\n", n->rate, n->streamLatMs.load(), g_padMs.load());
        t += b;
        SyncInfo si = GetSync(n);
        double total = g_baseMs.load() + n->delayMs.load() + n->trimMs.load() + si.total;
        swprintf(b, 500, L"Gesamtlatenz Quelle→Ohr ≈ %.1f ms (%ls)\r\n", total, SyncShort(si.src));
        t += b;
    } else if (n->kind == K_STREAM && n->st) {
        StreamCtx* s = n->st;
        swprintf(b, 500, L"Format: %ls / %d Hz Stereo\r\nPort %d · Clients: %d · gesendet: %.1f MB\r\nSonos: %ls\r\nSteuerung: %ls\r\n",
                 s->Flac() ? L"FLAC 24 Bit" : L"WAV 16 Bit", s->rate, s->port, s->nClients.load(), (double)s->bytesSent.load() / 1048576.0, s->room.empty() ? L"–" : s->room.c_str(),
                 s->msg[0] ? s->msg : L"–");
        t += b;
    } else if (n->kind == K_AIRPLAY && n->ap) {
        AirplayCtx* a = n->ap;
        std::wstring c;
        EnterCriticalSection(&a->cs);
        c = W8(a->clientIp);
        LeaveCriticalSection(&a->cs);
        swprintf(b, 500, L"AirPlay (RAOP) · ALAC 16 Bit / 44100 Hz\r\nName: %ls · Port %d\r\nClient: %ls\r\nPakete: %llu · verloren: %llu · wiederholt: %llu\r\nGemeldete Latenz: %d ms · iPhone-Lautstärke: %.1f dB\r\n",
                 a->name.c_str(), a->port, c.empty() ? L"–" : c.c_str(), (unsigned long long)a->packets.load(), (unsigned long long)a->lost.load(),
                 (unsigned long long)a->resent.load(), a->reportedLatMs.load(), a->volDb.load());
        t += b;
    } else if (n->kind == K_SUM) {
        t += L"Summen-Modul: kein eigener Takt, keine zusätzliche Latenz.\r\nKanal-Zuordnung je Eingangskabel (Inspector).\r\n";
    } else if (n->kind == K_REC && n->rc) {
        RecCtx* rc = n->rc;
        swprintf(b, 500, L"Aufnahme: %ls · %d Hz Stereo\r\nGeschrieben: %.1f MB (eine Datei)\r\n", RecFmtName(rc->bits, rc->flac), rc->rate, (double)rc->rec.bytes() / 1048576.0);
        t += b;
        if (rc->rec.overruns()) { swprintf(b, 500, L"ACHTUNG: %u Blöcke verworfen (Platte zu langsam)\r\n", rc->rec.overruns()); t += b; }
    } else if (n->kind != K_GEN) {
        swprintf(b, 500, L"Samplerate: %d Hz · Kanäle: %d\r\n", n->rate, n->ch);
        t += b;
    }
    if (IsSink(n->kind)) {
        for (Conn* c : g_conns) {
            if (c->dst != n) continue;
            swprintf(b, 500, L"<- %ls\r\n   Lag %.2f ms · Drift %+.1f ppm · Resyncs %d\r\n", c->src->name.c_str(), c->cs.lagMs, c->cs.corr * 1e6, c->cs.resyncs);
            t += b;
        }
    }
    wchar_t cur[3000] = {};
    GetWindowTextW(iInfo, cur, 3000);
    if (t != cur) SetWindowTextW(iInfo, t.c_str());
    if (n->kind == K_STREAM && n->st) {
        StreamCtx* s = n->st;
        std::wstring m;
        int ms = s->measState.load();
        if (!s->autoLat.load()) m = L"Positionsabfrage aus.";
        else if (ms == 2) {
            swprintf(b, 500, L"Gemessen: %.0f ms  ± %.0f ms\r\n(Sonos-Puffer + Vorlauf, n = %d)", s->measLatMs.load(), s->measErrMs.load(), s->measN.load());
            m = b;
        } else if (ms == 3) m = L"Sonos meldet keine laufende Position –\r\nbitte manuell einmessen (Feinkorrektur).";
        else if (s->ctl.load() == 2) {
            swprintf(b, 500, L"Messe … (%d Zeitmarken)", s->measN.load());
            m = b;
        } else m = L"Wartet auf Wiedergabe.";
        wchar_t cm[400] = {};
        GetWindowTextW(iMeas, cm, 400);
        if (m != cm) SetWindowTextW(iMeas, m.c_str());
    }
}

static void ApplyDelay(Node* n, double v) {
    v = std::max(0.0, std::min(MAX_DELAY_MS, v));
    n->delayMs = std::floor(v * 10.0 + 0.5) / 10.0;
}
static void ApplyTrim(Node* n, double v) {
    v = std::max(-200.0, std::min(200.0, v));
    n->trimMs = std::floor(v * 10.0 + 0.5) / 10.0;
}

static void ReopenNode(Node* n) {
    if (!g_running) return;
    EnterCriticalSection(&g_cs);
    CloseNode(n);
    LeaveCriticalSection(&g_cs);
}

// ======================================================================= UI: Canvas

static int g_dragKind = 0;                                // K_REC oder K_SUM: was wird gerade aus der Toolbar gezogen
static bool g_recDrag = false, g_ghostOn = false;       // Aufnahme-Box wird aus der Toolbar aufs Feld gezogen
static POINT g_ghostPx = {0, 0};
static double g_zoom = 1.0, g_panX = 0, g_panY = 0;       // Ansicht: unendliche Arbeitsflaeche
static inline double Sc() { return g_dpi * g_zoom / 96.0; }   // Pixel pro Welteinheit
static POINT g_panStartPx = {0, 0};
static double g_panOrigX = 0, g_panOrigY = 0;
static bool g_panRight = false;

struct DragState {
    int zone = 0;
    int mode = 0;  // 1 Node, 2 Kabel, 3 Zonen-Lautstaerke, 4 Ansicht verschieben
    Node* node = nullptr;
    float offX = 0, offY = 0;
    Node* from = nullptr;
    bool fromIsSrc = true;
    POINT mouse = {0, 0};
};
static DragState g_drag;
static int g_hoverPort = 0;

static POINT OutPort(Node* n) { return {(LONG)n->x + NODE_W, (LONG)n->y + 12}; }
static POINT InPort(Node* n) { return {(LONG)n->x, (LONG)n->y + 12}; }

static COLORREF KindColor(int k) {
    switch (k) {
        case K_LOOP: return RGB(64, 160, 255);
        case K_CAPTURE: return RGB(80, 210, 160);
        case K_GEN: return RGB(190, 120, 255);
        case K_STREAM: return RGB(60, 200, 215);
        case K_AIRPLAY: return RGB(255, 120, 190);
        case K_REC: return RGB(240, 70, 80);
        case K_SUM: return RGB(250, 190, 60);
        default: return RGB(255, 160, 60);
    }
}
static COLORREF Mix(COLORREF a, COLORREF b, int pct) {
    return RGB((GetRValue(a) * pct + GetRValue(b) * (100 - pct)) / 100, (GetGValue(a) * pct + GetGValue(b) * (100 - pct)) / 100,
               (GetBValue(a) * pct + GetBValue(b) * (100 - pct)) / 100);
}

static void Bezier(POINT a, POINT b, POINT pts[4]) {
    int d = std::max(60, (int)(std::abs(b.x - a.x) * 0.5));
    pts[0] = a;
    pts[1] = {a.x + d, a.y};
    pts[2] = {b.x - d, b.y};
    pts[3] = b;
}
static POINT BezPt(const POINT p[4], double t) {
    double u = 1 - t;
    double x = u * u * u * p[0].x + 3 * u * u * t * p[1].x + 3 * u * t * t * p[2].x + t * t * t * p[3].x;
    double y = u * u * u * p[0].y + 3 * u * u * t * p[1].y + 3 * u * t * t * p[2].y + t * t * t * p[3].y;
    return {(LONG)x, (LONG)y};
}
static float DbOf(float lin) { return lin > 0.00001f ? 20.0f * std::log10(lin) : -90.0f; }

static void DrawVU(HDC dc, int x, int y, int w, int h, float db, float hold) {
    RECT bg = {x, y, x + w, y + h};
    FillRectColor(dc, bg, TH.dark ? RGB(10, 12, 16) : RGB(214, 219, 229));
    const int segs = 60;
    float lvl = std::max(0.0f, std::min(1.0f, (db + 60.0f) / 60.0f));
    int lit = (int)(lvl * segs);
    float sw = (float)w / segs;
    for (int i = 0; i < segs; i++) {
        float segDb = -60.0f + (i + 0.5f) * 60.0f / segs;
        COLORREF c = segDb > -3 ? RGB(255, 60, 50) : segDb > -12 ? RGB(250, 205, 60) : RGB(60, 215, 110);
        if (i >= lit) c = Mix(c, TH.dark ? RGB(10, 12, 16) : RGB(214, 219, 229), 22);
        RECT r = {x + (int)(i * sw) + 1, y + 1, x + (int)((i + 1) * sw), y + h - 1};
        FillRectColor(dc, r, c);
    }
    float hl = std::max(0.0f, std::min(1.0f, (hold + 60.0f) / 60.0f));
    if (hold > -59) {
        RECT r = {x + (int)(hl * w) - 1, y, x + (int)(hl * w) + 1, y + h};
        FillRectColor(dc, r, TH.dark ? RGB(240, 240, 240) : RGB(40, 44, 52));
    }
}

static void Txt(HDC dc, const std::wstring& s, int x, int y, int w, int h, UINT fmt, COLORREF col, HFONT f) {
    SelectObject(dc, f);
    SetTextColor(dc, col);
    RECT r = {x, y, x + w, y + h};
    DrawTextW(dc, s.c_str(), -1, &r, fmt | DT_NOPREFIX);
}

static std::wstring SumStatusLine(Node* n) {
    int ins = 0, outs = 0;
    for (Conn* c : g_conns) { if (c->dst == n) ins++; if (c->src == n) outs++; }
    wchar_t b[120];
    swprintf(b, 120, L"Summe · %d Eingang/Eingänge → %d Ausgang/Ausgänge%ls", ins, outs, n->sumMono.load() ? L" · Mono" : L"");
    return b;
}

static void DrawRecButton(HDC dc, Node* n, int x, int y, int w, int h) {
    RecCtx* rc = n->rc;
    bool on = rc && rc->rec.active();
    bool blink = on && ((int)(NowSec() * 2) % 2 == 0);
    RECT r = {x, y, x + w, y + h};
    FillRectColor(dc, r, on ? (blink ? RGB(235, 40, 50) : RGB(170, 28, 36)) : (TH.dark ? RGB(96, 34, 40) : RGB(236, 196, 200)));
    std::wstring t = L"● REC";
    if (on) {
        double s = (double)rc->rec.frames() / std::max(1, rc->rate);
        wchar_t b[48];
        swprintf(b, 48, L"■ %02d:%02d", (int)(s / 60), (int)s % 60);
        t = b;
    }
    Txt(dc, t, x, y + 1, w, h - 2, DT_CENTER | DT_VCENTER | DT_SINGLELINE, on ? RGB(255, 255, 255) : (TH.dark ? RGB(255, 140, 146) : RGB(160, 20, 30)), g_cBold);
}

static void DrawNode(HDC dc, Node* n, bool selected) {
    int x = (int)n->x, y = (int)n->y;
    COLORREF kc = KindColor(n->kind);
    const int NH = NodeH(n);
    RECT body = {x, y, x + NODE_W, y + NH};
    FillRectColor(dc, body, TH.nodeBody);
    RECT head = {x, y, x + NODE_W, y + 24};
    FillRectColor(dc, head, TH.dark ? Mix(kc, TH.nodeBody, 38) : Mix(kc, RGB(255, 255, 255), 30));
    HPEN pen = CreatePen(PS_SOLID, selected ? 2 : 1, selected ? TH.text : kc);
    HGDIOBJ op = SelectObject(dc, pen), obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, x, y, x + NODE_W + 1, y + NH + 1);
    SelectObject(dc, op);
    SelectObject(dc, obr);
    DeleteObject(pen);

    Txt(dc, n->name, x + 8, y + 4, NODE_W - 80, 18, DT_LEFT | DT_END_ELLIPSIS | DT_SINGLELINE, TH.dark ? RGB(240, 244, 250) : RGB(20, 24, 32), g_cBold);
    RECT mr = {x + NODE_W - 56, y + 4, x + NODE_W - 36, y + 20};
    bool muted = n->mute.load();
    FillRectColor(dc, mr, muted ? RGB(220, 50, 50) : (TH.dark ? RGB(50, 56, 70) : RGB(200, 206, 220)));
    Txt(dc, L"M", mr.left, mr.top, 20, 16, DT_CENTER | DT_VCENTER | DT_SINGLELINE, muted ? RGB(255, 255, 255) : TH.nodeText, g_cUI);

    int st = n->state.load();
    COLORREF sc = !g_running ? RGB(120, 126, 138) : st == 1 ? RGB(70, 230, 120) : st == 2 ? RGB(255, 70, 70) : RGB(250, 200, 60);
    HBRUSH sb = CreateSolidBrush(sc);
    HGDIOBJ ob2 = SelectObject(dc, sb), opn = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, x + NODE_W - 28, y + 7, x + NODE_W - 16, y + 19);
    SelectObject(dc, ob2);
    SelectObject(dc, opn);
    DeleteObject(sb);

    COLORREF dim = TH.nodeDim;
    Txt(dc, L"L", x + 8, y + 30, 14, 12, DT_LEFT | DT_SINGLELINE, dim, g_cMono);
    Txt(dc, L"R", x + 8, y + 46, 14, 12, DT_LEFT | DT_SINGLELINE, dim, g_cMono);
    int vx = x + 22, vw = NODE_W - 22 - 74;
    DrawVU(dc, vx, y + 30, vw, 12, n->dispL, n->holdL);
    DrawVU(dc, vx, y + 46, vw, 12, n->dispR, n->holdR);
    wchar_t b[96];
    swprintf(b, 64, n->dispL > -89 ? L"%.1f" : L"-inf", n->dispL);
    Txt(dc, b, x + NODE_W - 70, y + 29, 40, 14, DT_RIGHT | DT_SINGLELINE, TH.nodeText, g_cMono);
    swprintf(b, 64, n->dispR > -89 ? L"%.1f" : L"-inf", n->dispR);
    Txt(dc, b, x + NODE_W - 70, y + 45, 40, 14, DT_RIGHT | DT_SINGLELINE, TH.nodeText, g_cMono);
    bool clip = n->clip.load();
    RECT cr = {x + NODE_W - 26, y + 32, x + NODE_W - 12, y + 54};
    FillRectColor(dc, cr, clip ? RGB(255, 40, 40) : (TH.dark ? RGB(60, 30, 32) : RGB(236, 210, 212)));
    Txt(dc, L"CLP", cr.left - 2, cr.top + 5, 18, 12, DT_CENTER | DT_SINGLELINE, clip ? RGB(255, 255, 255) : dim, g_cSmall);

    static const int ticks[] = {-60, -40, -20, -12, -6, 0};
    for (int t : ticks) {
        int tx = vx + (int)((t + 60) / 60.0 * vw);
        HPEN tp = CreatePen(PS_SOLID, 1, dim);
        HGDIOBJ otp = SelectObject(dc, tp);
        MoveToEx(dc, tx, y + 60, nullptr);
        LineTo(dc, tx, y + 64);
        SelectObject(dc, otp);
        DeleteObject(tp);
        swprintf(b, 64, L"%d", t);
        Txt(dc, b, tx - 14, y + 64, 28, 11, DT_CENTER | DT_SINGLELINE, dim, g_cSmall);
    }

    swprintf(b, 64, L"Gain %+.1f dB", n->gainDb);
    Txt(dc, b, x + 8, y + 80, 120, 14, DT_LEFT | DT_SINGLELINE, TH.nodeText, g_cMono);
    double dly = n->delayMs.load() + n->trimMs.load();
    if (n->kind == K_REC) DrawRecButton(dc, n, x + 130, y + 77, 136, 18);
    else {
        swprintf(b, 64, L"Delay %.1f ms", dly);
        Txt(dc, b, x + 130, y + 80, 136, 14, DT_LEFT | DT_SINGLELINE, TH.dark ? RGB(255, 220, 120) : RGB(170, 110, 0), g_cMono);
    }
    std::wstring l2;
    if (n->kind == K_STREAM) l2 = StreamStatusLine(n);
    else if (n->kind == K_AIRPLAY) l2 = ApStatusLine(n);
    else if (n->kind == K_REC) l2 = RecStatusLine(n);
    else if (n->kind == K_SUM) l2 = SumStatusLine(n);
    else if (st == 2) l2 = std::wstring(L"! ") + n->err;
    else if (n->kind == K_GEN) l2 = L"48000 Hz · Testsignal";
    else if (n->rate > 0 && g_running) {
        swprintf(b, 64, L"%d Hz", n->rate);
        l2 = b;
        if (n->kind == K_OUT) {
            SyncInfo si = GetSync(n);
            swprintf(b, 64, L" · %.0f ms (%ls)", g_baseMs.load() + dly + si.total, SyncShort(si.src));
            l2 += b;
        }
    } else l2 = IsSink(n->kind) ? L"Ausgang" : L"Quelle";
    bool bad = st == 2 || (n->kind == K_STREAM && n->st && n->st->ctl.load() == 3);
    Txt(dc, l2, x + 8, y + 98, NODE_W - 16, 14, DT_LEFT | DT_END_ELLIPSIS | DT_SINGLELINE, bad ? RGB(255, 110, 100) : dim, g_cMono);

    if (ZoneCount(n) > 0) {   // Sonos-Gruppe: je Zone Status, Pegel (zeitversetzt) und Lautstaerkeregler
        StreamCtx* sc = n->st;
        int y0 = y + NODE_H;
        HPEN sp = CreatePen(PS_SOLID, 1, TH.dark ? RGB(60, 66, 80) : RGB(210, 214, 224));
        HGDIOBJ osp = SelectObject(dc, sp);
        MoveToEx(dc, x, y0, nullptr);
        LineTo(dc, x + NODE_W, y0);
        SelectObject(dc, osp);
        DeleteObject(sp);
        Txt(dc, L"Sonos-Zonen · Lautstärke & Pegel (zeitversetzt)", x + 8, y0 + 2, NODE_W - 16, 12, DT_LEFT | DT_SINGLELINE, dim, g_cSmall);
        for (int z = 0; z < ZoneCount(n); z++) {
            int ry = y0 + MEM_HDR + z * MEM_ROW;
            std::wstring nm, sub;
            COLORREF dc2;
            if (z == 0) {
                int cst = sc->ctl.load();
                dc2 = !g_running ? RGB(120, 126, 138) : cst == 2 ? RGB(70, 230, 120) : cst == 3 ? RGB(255, 70, 70) : RGB(250, 200, 60);
                nm = !sc->room.empty() ? CleanText(sc->room) : W8(sc->ip);
                sub = W8(sc->ip) + L" · Haupt-Zone";
            } else {
                size_t i = (size_t)z - 1;
                int stt = i < sc->groupStat.size() ? sc->groupStat[i] : 0;
                dc2 = !g_running ? RGB(120, 126, 138) : stt == 1 ? RGB(70, 230, 120) : stt == 2 ? RGB(255, 70, 70) : RGB(250, 200, 60);
                nm = i < sc->groupNames.size() ? CleanText(sc->groupNames[i]) : L"";
                if (nm.empty()) nm = W8(sc->groupIps[i]);
                const wchar_t* stx = !g_running ? L"" : stt == 1 ? L"in Gruppe" : stt == 2 ? L"nicht beigetreten" : L"prüfe …";
                sub = W8(sc->groupIps[i]) + (stx[0] ? L" · " : L"") + stx;
            }
            HBRUSH db = CreateSolidBrush(dc2);
            HGDIOBJ odb = SelectObject(dc, db), odp = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, x + 7, ry + 9, x + 15, ry + 17);
            SelectObject(dc, odb);
            SelectObject(dc, odp);
            DeleteObject(db);
            Txt(dc, nm, x + 20, ry + 3, 112, 14, DT_LEFT | DT_END_ELLIPSIS | DT_SINGLELINE, TH.nodeText, g_cUI);
            Txt(dc, sub, x + 20, ry + 17, 114, 12, DT_LEFT | DT_END_ELLIPSIS | DT_SINGLELINE, dim, g_cSmall);
            float ml = (size_t)z < n->mDispL.size() ? n->mDispL[z] : -90, mr = (size_t)z < n->mDispR.size() ? n->mDispR[z] : -90;
            DrawVU(dc, x + 138, ry + 6, NODE_W - 138 - 38, 8, ml, -90);
            DrawVU(dc, x + 138, ry + 17, NODE_W - 138 - 38, 8, mr, -90);
            float mx = std::max(ml, mr);
            swprintf(b, 64, mx > -89 ? L"%.0f" : L"-inf", mx);
            Txt(dc, b, x + NODE_W - 34, ry + 9, 28, 14, DT_RIGHT | DT_SINGLELINE, TH.nodeText, g_cMono);
            // Lautstaerkeregler
            int vol = ZoneVol(sc, z);
            int sx0 = x + 20, sx1 = x + NODE_W - 44, sy = ry + 36;
            HPEN tp = CreatePen(PS_SOLID, 3, TH.dark ? RGB(58, 64, 80) : RGB(200, 206, 220));
            HGDIOBJ otp = SelectObject(dc, tp);
            MoveToEx(dc, sx0, sy, nullptr);
            LineTo(dc, sx1, sy);
            SelectObject(dc, otp);
            DeleteObject(tp);
            if (vol >= 0) {
                int fx = sx0 + (sx1 - sx0) * vol / 100;
                HPEN fp = CreatePen(PS_SOLID, 3, TH.accent);
                HGDIOBJ ofp = SelectObject(dc, fp);
                MoveToEx(dc, sx0, sy, nullptr);
                LineTo(dc, fx, sy);
                SelectObject(dc, ofp);
                DeleteObject(fp);
                HBRUSH tb = CreateSolidBrush(TH.accent);
                HGDIOBJ otb = SelectObject(dc, tb), onp = SelectObject(dc, GetStockObject(NULL_PEN));
                Ellipse(dc, fx - 5, sy - 5, fx + 6, sy + 6);
                SelectObject(dc, otb);
                SelectObject(dc, onp);
                DeleteObject(tb);
            }
            swprintf(b, 64, vol >= 0 ? L"%d%%" : L"–", vol);
            Txt(dc, b, x + NODE_W - 40, ry + 29, 34, 14, DT_RIGHT | DT_SINGLELINE, TH.nodeText, g_cMono);
        }
    }

    for (int pi = 0; pi < (n->kind == K_SUM ? 2 : 1); pi++) {   // Summen-Modul: Eingang links und Ausgang rechts
        POINT p = n->kind == K_SUM ? (pi == 0 ? InPort(n) : OutPort(n)) : (IsSink(n->kind) ? InPort(n) : OutPort(n));
        bool hot = (g_hoverNode == n && g_hoverPort != 0 && (n->kind != K_SUM || g_hoverPort == (pi == 0 ? 2 : 1)));
        HBRUSH pb = CreateSolidBrush(hot ? RGB(255, 255, 255) : kc);
        HGDIOBJ opb = SelectObject(dc, pb);
        HPEN pp = CreatePen(PS_SOLID, 2, TH.canvasBg);
        HGDIOBJ opp = SelectObject(dc, pp);
        Ellipse(dc, p.x - 7, p.y - 7, p.x + 8, p.y + 8);
        SelectObject(dc, opb);
        SelectObject(dc, opp);
        DeleteObject(pb);
        DeleteObject(pp);
    }
}

static void PaintCanvas(HDC wdc, RECT rc) {
    int W = rc.right, H = rc.bottom;
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, W, H);
    HGDIOBJ obmp = SelectObject(dc, bmp);
    FillRectColor(dc, rc, TH.canvasBg);
    SetMapMode(dc, MM_ANISOTROPIC);
    SetWindowExtEx(dc, 10000, 10000, nullptr);
    long vxe = std::lround(Sc() * 10000.0);
    SetViewportExtEx(dc, vxe, vxe, nullptr);
    SetWindowOrgEx(dc, (int)std::lround(g_panX), (int)std::lround(g_panY), nullptr);
    SetBkMode(dc, TRANSPARENT);
    {   // unendliches Raster
        double wv = W / Sc(), hv = H / Sc();
        int step = 20;
        while (step * Sc() < 14.0) step *= 2;
        int gx0 = (int)std::floor(g_panX / step) * step, gy0 = (int)std::floor(g_panY / step) * step;
        for (int gx = gx0; gx < g_panX + wv; gx += step)
            for (int gy = gy0; gy < g_panY + hv; gy += step) SetPixel(dc, gx, gy, TH.grid);
    }

    for (Conn* c : g_conns) {
        POINT bz[4];
        Bezier(OutPort(c->src), InPort(c->dst), bz);
        bool sel = (c == g_selConn);
        COLORREF col = KindColor(c->src->kind);
        if (sel) {
            HPEN wp = CreatePen(PS_SOLID, 7, TH.text);
            HGDIOBJ ow = SelectObject(dc, wp);
            PolyBezier(dc, bz, 4);
            SelectObject(dc, ow);
            DeleteObject(wp);
        }
        HPEN pn = CreatePen(PS_SOLID, 4, col);
        HGDIOBJ op = SelectObject(dc, pn);
        PolyBezier(dc, bz, 4);
        SelectObject(dc, op);
        DeleteObject(pn);
        if (c->cs.init) {
            POINT m = BezPt(bz, 0.5);
            wchar_t b[40];
            swprintf(b, 40, L"%.0f ms", c->cs.lagMs);
            Txt(dc, b, m.x - 28, m.y - 18, 56, 14, DT_CENTER | DT_SINGLELINE, TH.nodeDim, g_cMono);
        }
    }
    if (g_drag.mode == 2 && g_drag.from) {
        POINT a = g_drag.fromIsSrc ? OutPort(g_drag.from) : g_drag.mouse;
        POINT b2 = g_drag.fromIsSrc ? g_drag.mouse : InPort(g_drag.from);
        POINT bz[4];
        Bezier(a, b2, bz);
        HPEN pn = CreatePen(PS_DOT, 1, TH.text);
        HGDIOBJ op = SelectObject(dc, pn);
        PolyBezier(dc, bz, 4);
        SelectObject(dc, op);
        DeleteObject(pn);
    }
    for (Node* n : g_nodes) DrawNode(dc, n, n == g_sel);
    if (g_recDrag && g_ghostOn) {   // Geisterbild der Aufnahme-Box beim Ziehen aus der Toolbar
        int gx = (int)(std::floor((g_ghostPx.x / Sc() + g_panX - NODE_W / 2) / 10.0) * 10), gy = (int)(std::floor((g_ghostPx.y / Sc() + g_panY - 12) / 10.0) * 10);
        HPEN gp = CreatePen(PS_DASH, 2, RGB(240, 70, 80));
        HGDIOBJ gop = SelectObject(dc, gp), gob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, gx, gy, gx + NODE_W, gy + NODE_H);
        SelectObject(dc, gop); SelectObject(dc, gob); DeleteObject(gp);
        Txt(dc, g_dragKind == K_SUM ? L"∑ Summen-Modul – hier loslassen" : L"● Aufnahme-Box – hier loslassen", gx + 8, gy + 8, NODE_W - 16, 18, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, RGB(255, 130, 138), g_cBold);
    }
    if (g_nodes.empty()) {
        Txt(dc, L"Leer. Oben „+ Quelle“, „+ Ausgang“ oder „+ Stream-Ausgang“ wählen (oder „Virtuelles Gerät…“ für den Assistenten),\n"
                L"dann am Port (Kreis) ziehen und die Kabel verbinden. Mausrad = Zoom · Alt+Mausrad = Delay ±1 ms · Rechtsklick auf Kabel = trennen · Entf = löschen",
            24, 24, 760, 50, DT_LEFT, TH.nodeDim, g_cUI);
    }
    SetMapMode(dc, MM_TEXT);
    SetViewportExtEx(dc, 1, 1, nullptr);
    SetWindowExtEx(dc, 1, 1, nullptr);
    SetWindowOrgEx(dc, 0, 0, nullptr);
    {   // Overlay in Bildschirmkoordinaten: Zoom-Hinweis und Copyright
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, TH.nodeDim);
        wchar_t zb[200];
        swprintf(zb, 200, L"Zoom %.0f %%  ·  Mausrad = Zoom  ·  Ziehen = Verschieben  ·  F = alles anzeigen  ·  0 = 100 %%", g_zoom * 100.0);
        RECT zr = {S(10), H - S(20), W - S(10), H - S(2)};
        DrawTextW(dc, zb, -1, &zr, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
        RECT cr2 = {S(10), H - S(20), W - S(10), H - S(2)};
        DrawTextW(dc, APP_COPYRIGHT, -1, &cr2, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
    }
    BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, obmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static POINT ToWorldPx(int px, int py) { return {(LONG)std::floor(px / Sc() + g_panX), (LONG)std::floor(py / Sc() + g_panY)}; }
static POINT ToLogical(LPARAM lp) { return ToWorldPx((short)LOWORD(lp), (short)HIWORD(lp)); }

static void FitView() {
    RECT rc;
    GetClientRect(g_canvas, &rc);
    if (g_nodes.empty() || rc.right < 50 || rc.bottom < 50) { g_zoom = 1.0; g_panX = g_panY = 0; return; }
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (Node* n : g_nodes) {
        x0 = std::min(x0, (double)n->x);
        y0 = std::min(y0, (double)n->y);
        x1 = std::max(x1, (double)n->x + NODE_W);
        y1 = std::max(y1, (double)n->y + NodeH(n));
    }
    double m = 40, bw = x1 - x0 + 2 * m, bh = y1 - y0 + 2 * m;
    double sc = std::min(rc.right / bw, rc.bottom / bh);
    g_zoom = std::max(0.15, std::min(2.0, sc * 96.0 / g_dpi));
    g_panX = x0 - m - (rc.right / Sc() - bw) / 2;
    g_panY = y0 - m - (rc.bottom / Sc() - bh) / 2;
}

static void PlaceNodeAuto(Node* n, bool sink) {   // neue Nodes innerhalb der aktuellen Ansicht platzieren
    RECT rc;
    GetClientRect(g_canvas, &rc);
    double vw = rc.right / Sc();
    int cnt = 0;
    for (Node* o : g_nodes) if (o != n && IsSink(o->kind) == sink) cnt++;
    n->x = (float)(std::floor((sink ? g_panX + std::max(330.0, vw - NODE_W - 40) : g_panX + 40) / 10.0) * 10.0);
    n->y = (float)(std::floor((g_panY + 30 + (cnt % 4) * (NODE_H + 60.0) + (cnt / 4) * 20.0) / 10.0) * 10.0);
}

static bool InRect(POINT p, int x, int y, int w, int h);
static int VolFromX(Node* n, int xw) {
    int sx0 = (int)n->x + 20, sx1 = (int)n->x + NODE_W - 44;
    return std::max(0, std::min(100, (int)std::lround((double)(xw - sx0) * 100.0 / (double)(sx1 - sx0))));
}
static bool HitVol(Node* n, POINT p, int& zone, int& val) {
    for (int z = 0; z < ZoneCount(n); z++) {
        int ry = (int)n->y + NODE_H + MEM_HDR + z * MEM_ROW;
        if (InRect(p, (int)n->x + 14, ry + 27, NODE_W - 52, 18)) { zone = z; val = VolFromX(n, p.x); return true; }
    }
    return false;
}
static bool InRect(POINT p, int x, int y, int w, int h) { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }

static Node* HitNode(POINT p) {
    for (size_t i = g_nodes.size(); i-- > 0;) {
        Node* n = g_nodes[i];
        if (InRect(p, (int)n->x - 10, (int)n->y, NODE_W + 20, NodeH(n))) return n;
    }
    return nullptr;
}
static int HitPort(Node* n, POINT p) {   // 1 = Ausgangs-Port (Quelle), 2 = Eingangs-Port (Senke); Summen-Modul hat beide
    double hr = 12.0 / std::min(1.0, g_zoom);
    auto near_ = [&](POINT q) { int dx = p.x - q.x, dy = p.y - q.y; return (double)(dx * dx + dy * dy) <= hr * hr; };
    if (!IsSink(n->kind) && near_(OutPort(n))) return 1;
    if ((IsSink(n->kind) || n->kind == K_SUM) && near_(InPort(n))) return 2;
    return 0;
}
static Conn* HitCable(POINT p) {
    for (size_t i = g_conns.size(); i-- > 0;) {
        Conn* c = g_conns[i];
        POINT bz[4];
        Bezier(OutPort(c->src), InPort(c->dst), bz);
        for (int k = 0; k <= 60; k++) {
            POINT q = BezPt(bz, k / 60.0);
            int dx = q.x - p.x, dy = q.y - p.y;
            if (dx * dx + dy * dy <= 7 * 7) return c;
        }
    }
    return nullptr;
}

static void SelectNode(Node* n, Conn* c) {
    g_sel = n;
    g_selConn = c;
    g_scrollY = 0;
    UpdateInspector();
}

static bool TryConnect(Node* src, Node* dst) {
    if (!src || !dst || IsSink(src->kind) || !(IsSink(dst->kind) || dst->kind == K_SUM)) return false;
    if (src->kind == K_SUM && dst->kind == K_SUM) { g_msg = L"Abgelehnt: Summen-Module lassen sich nicht hintereinander schalten."; return false; }
    if (FindConn(src, dst)) return false;
    if (src->kind == K_LOOP && dst->kind == K_OUT && src->devId == dst->devId) {
        g_msg = L"Abgelehnt: Mitschnitt und Ausgang sind dasselbe Gerät (Rückkopplung). Player auf ein anderes Gerät (z. B. Virtual 3/4) legen.";
        return false;
    }
    Conn* c = new Conn();
    c->src = src;
    c->dst = dst;
    if (dst->kind == K_SUM) {   // Standard: Mono-Quellen -> erste links, zweite rechts, weitere in die Mitte; bekannte Stereo-Quellen unveraendert
        int cnt = 0;
        for (Conn* o : g_conns) if (o->dst == dst) cnt++;
        if (src->ch >= 2) { c->srcCh = 0; c->dstCh = 0; }
        else { c->srcCh = 3; c->dstCh = cnt == 0 ? 1 : cnt == 1 ? 2 : 3; }
    }
    EnterCriticalSection(&g_cs);
    g_conns.push_back(c);
    LeaveCriticalSection(&g_cs);
    SaveGraph();
    return true;
}

static void DeleteSelection() {
    if (g_selConn) {
        Conn* c = g_selConn;
        g_selConn = nullptr;
        RemoveConn(c);
        SaveGraph();
    } else if (g_sel) {
        Node* n = g_sel;
        g_sel = nullptr;
        RemoveNode(n);
        UpdateInspector();
        SaveGraph();
    }
}

static LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        PaintCanvas(dc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        POINT p = ToLogical(lp);
        g_drag.mouse = p;
        for (size_t i = g_nodes.size(); i-- > 0;) {
            Node* n = g_nodes[i];
            int hpt = HitPort(n, p);
            if (hpt) {
                g_drag.mode = 2;
                g_drag.from = n;
                g_drag.fromIsSrc = hpt == 1;
                SetCapture(hwnd);
                SelectNode(n, nullptr);
                return 0;
            }
        }
        Node* n = HitNode(p);
        if (n) {
            if (InRect(p, (int)n->x + NODE_W - 56, (int)n->y + 4, 20, 16)) {
                n->mute = !n->mute.load();
                SelectNode(n, nullptr);
                SaveGraph();
                return 0;
            }
            if (n->kind == K_REC && InRect(p, (int)n->x + 130, (int)n->y + 77, 136, 18)) {   // REC-Knopf direkt am Node
                SelectNode(n, nullptr);
                RecToggle(n);
                if (n->rc && !n->rc->rec.active() && !n->rc->err.empty()) g_msg = n->rc->err;
                RefreshInspectorInfo();
                return 0;
            }
            if (InRect(p, (int)n->x + NODE_W - 26, (int)n->y + 32, 14, 22)) { n->clip = false; return 0; }
            {   // Zonen-Lautstaerke direkt im Dashboard
                int z = 0, v = 0;
                if (n->st && HitVol(n, p, z, v)) {
                    g_drag.mode = 3;
                    g_drag.node = n;
                    g_drag.zone = z;
                    SetCapture(hwnd);
                    SetZoneWant(n->st, z, v);
                    return 0;
                }
            }
            EnterCriticalSection(&g_cs);
            g_nodes.erase(std::remove(g_nodes.begin(), g_nodes.end(), n), g_nodes.end());
            g_nodes.push_back(n);
            LeaveCriticalSection(&g_cs);
            g_drag.mode = 1;
            g_drag.node = n;
            g_drag.offX = p.x - n->x;
            g_drag.offY = p.y - n->y;
            SetCapture(hwnd);
            if (n != g_sel) SelectNode(n, nullptr);
            return 0;
        }
        Conn* c = HitCable(p);
        SelectNode(nullptr, c);
        if (!c) {   // leere Flaeche: Ansicht verschieben
            g_drag.mode = 4;
            g_panStartPx = {(short)LOWORD(lp), (short)HIWORD(lp)};
            g_panOrigX = g_panX;
            g_panOrigY = g_panY;
            g_panRight = false;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MBUTTONDOWN:
        SetFocus(hwnd);
        g_drag.mode = 4;
        g_panStartPx = {(short)LOWORD(lp), (short)HIWORD(lp)};
        g_panOrigX = g_panX;
        g_panOrigY = g_panY;
        g_panRight = true;
        SetCapture(hwnd);
        return 0;
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        if (g_drag.mode == 4 && g_panRight) { g_drag.mode = 0; ReleaseCapture(); }
        return 0;
    case WM_MOUSEMOVE: {
        POINT p = ToLogical(lp);
        if (g_drag.mode == 4) {
            int px = (short)LOWORD(lp), py = (short)HIWORD(lp);
            g_panX = g_panOrigX - (px - g_panStartPx.x) / Sc();
            g_panY = g_panOrigY - (py - g_panStartPx.y) / Sc();
            return 0;
        }
        if (g_drag.mode == 3 && g_drag.node && g_drag.node->st) {
            SetZoneWant(g_drag.node->st, g_drag.zone, VolFromX(g_drag.node, p.x));
            return 0;
        }
        g_drag.mouse = p;
        if (g_drag.mode == 1 && g_drag.node) {
            g_drag.node->x = std::floor((p.x - g_drag.offX) / 10.0f + 0.5f) * 10.0f;
            g_drag.node->y = std::floor((p.y - g_drag.offY) / 10.0f + 0.5f) * 10.0f;
        }
        g_hoverNode = nullptr;
        g_hoverPort = 0;
        for (size_t i = g_nodes.size(); i-- > 0;) {
            int hp = HitPort(g_nodes[i], p);
            if (hp) { g_hoverNode = g_nodes[i]; g_hoverPort = hp; break; }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p = ToLogical(lp);
        if (g_drag.mode == 2 && g_drag.from) {
            for (Node* n : g_nodes) {
                if (n == g_drag.from) continue;
                if (HitPort(n, p) || InRect(p, (int)n->x, (int)n->y, NODE_W, NodeH(n))) {
                    if (g_drag.fromIsSrc) TryConnect(g_drag.from, n);
                    else TryConnect(n, g_drag.from);
                    break;
                }
            }
        }
        if (g_drag.mode == 1) SaveGraph();
        g_drag.mode = 0;
        g_drag.from = nullptr;
        g_drag.node = nullptr;
        ReleaseCapture();
        return 0;
    }
    case WM_RBUTTONDOWN: {
        POINT p = ToLogical(lp);
        Conn* c = HitCable(p);
        if (c && !HitNode(p)) {
            if (g_selConn == c) g_selConn = nullptr;
            RemoveConn(c);
            SaveGraph();
            return 0;
        }
        Node* n = HitNode(p);
        if (n) {
            SelectNode(n, nullptr);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, n->mute.load() ? L"Mute aufheben" : L"Stummschalten");
            AppendMenuW(m, MF_STRING, 2, L"Neu verbinden");
            AppendMenuW(m, MF_STRING, 3, L"Alle Kabel dieses Nodes trennen");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 4, L"Node entfernen");
            POINT sp = {(short)LOWORD(lp), (short)HIWORD(lp)};
            ClientToScreen(hwnd, &sp);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, sp.x, sp.y, 0, g_hwnd, nullptr);
            DestroyMenu(m);
            if (cmd == 1) { n->mute = !n->mute.load(); UpdateInspector(); SaveGraph(); }
            else if (cmd == 2) ReopenNode(n);
            else if (cmd == 3) {
                for (size_t i = 0; i < g_conns.size();) {
                    if (g_conns[i]->src == n || g_conns[i]->dst == n) { Conn* c2 = g_conns[i]; if (g_selConn == c2) g_selConn = nullptr; RemoveConn(c2); }
                    else i++;
                }
                SaveGraph();
            } else if (cmd == 4) { g_sel = n; g_selConn = nullptr; DeleteSelection(); }
        } else {
            SetFocus(hwnd);
            g_drag.mode = 4;
            g_panStartPx = {(short)LOWORD(lp), (short)HIWORD(lp)};
            g_panOrigX = g_panX;
            g_panOrigY = g_panY;
            g_panRight = true;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT sp = {(short)LOWORD(lp), (short)HIWORD(lp)};
        ScreenToClient(hwnd, &sp);
        POINT p = ToWorldPx(sp.x, sp.y);
        Node* n = HitNode(p);
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (alt && n && n->kind != K_REC && !(g_autoSync && IsSink(n->kind))) {   // Alt + Rad auf einem Node: Verzoegerung
            double step = (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) ? 10.0 : (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) ? 0.1 : 1.0;
            int dir = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
            ApplyDelay(n, n->delayMs.load() + dir * step);
            if (n == g_sel) UpdateInspector();
        } else {   // Zoom auf den Mauszeiger
            double f = std::pow(1.12, GET_WHEEL_DELTA_WPARAM(wp) / 120.0);
            double wx = sp.x / Sc() + g_panX, wy = sp.y / Sc() + g_panY;
            g_zoom = std::max(0.15, std::min(4.0, g_zoom * f));
            g_panX = wx - sp.x / Sc();
            g_panY = wy - sp.y / Sc();
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_DELETE) DeleteSelection();
        else if (wp == 'F' || wp == VK_HOME) FitView();
        else if (wp == '0' || wp == VK_NUMPAD0) { g_zoom = 1.0; }
        return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ======================================================================= UI: Timer, Status, Theme

static void UpdateMeters(double now, double dt) {
    for (Node* n : g_nodes) {
        float l = DbOf(n->pkL.exchange(0.0f)), r = DbOf(n->pkR.exchange(0.0f));
        float fall = 22.0f * (float)dt;
        n->dispL = std::max(l, n->dispL - fall);
        n->dispR = std::max(r, n->dispR - fall);
        if (l >= n->holdL) { n->holdL = l; n->holdTL = now; }
        else if (now - n->holdTL > 1.5) n->holdL = std::max(l, n->holdL - 40.0f * (float)dt);
        if (r >= n->holdR) { n->holdR = r; n->holdTR = now; }
        else if (now - n->holdTR > 1.5) n->holdR = std::max(r, n->holdR - 40.0f * (float)dt);
        if (n->st) {
            size_t cnt = (size_t)ZoneCount(n);
            n->histT[n->histN % 512] = now;
            n->histL[n->histN % 512] = l;
            n->histR[n->histN % 512] = r;
            n->histN++;
            if (n->mDispL.size() != cnt) { n->mDispL.assign(cnt, -90.0f); n->mDispR.assign(cnt, -90.0f); }
            if (cnt) {
                double tt = now - GetSync(n).total / 1000.0;   // was jetzt hoerbar ist, wurde vor <Latenz> erzeugt
                float ml = -90, mr = -90;
                size_t m = std::min<size_t>(n->histN, 512);
                for (size_t j = 0; j < m; j++) {
                    size_t k = (n->histN - 1 - j) % 512;
                    if (n->histT[k] <= tt) { ml = n->histL[k]; mr = n->histR[k]; break; }
                }
                for (size_t i = 0; i < cnt; i++) {
                    n->mDispL[i] = std::max(ml, n->mDispL[i] - fall);
                    n->mDispR[i] = std::max(mr, n->mDispR[i] - fall);
                }
            }
        }
    }
}

static void InvalidateStatus() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    RECT sr = {0, rc.bottom - S(STATUS_H), rc.right, rc.bottom};
    InvalidateRect(g_hwnd, &sr, FALSE);
}

static void UpdateStatusBar() {
    int ok = 0, err = 0;
    for (Node* n : g_nodes) { if (n->state.load() == 1) ok++; else if (n->state.load() == 2) err++; }
    wchar_t b[300];
    g_stText[0] = g_running ? L"●  Läuft" : L"○  Gestoppt";
    swprintf(b, 300, L"Nodes %d (%d ok, %d Fehler)  ·  Kabel %d  ·  Loop %.0f µs  ·  Underruns %u  ·  Resampler: Sinc 90 dB", (int)g_nodes.size(), ok, err,
             (int)g_conns.size(), g_loopUs.load(), g_underruns.load());
    g_stText[1] = b;
    g_stText[2] = g_msg.empty() ? L"Tipp: Mausrad = Zoom · Alt+Mausrad auf Node = Delay · Rechtsklick = Menü" : g_msg;
    InvalidateStatus();
}

static void ApplyThemeNow() {
    bool dark = g_themeMode == 1 ? true : g_themeMode == 2 ? false : SystemUsesDark();
    SetThemeColors(dark);
    ApplyDarkMenus(dark);
    ApplyWindowChrome(g_hwnd);
    for (auto& nc : g_native) ThemeNative(nc.h, nc.type);
    for (HWND c : {g_cmbPad, g_cmbBase, g_cmbPoll, g_cmbDrv}) ThemeNative(c, 1);
    RedrawWindow(g_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

static void LoadSettings() {
    wchar_t b[64] = {};
    GetPrivateProfileStringW(L"UI", L"Theme", L"auto", b, 64, SettingsPath().c_str());
    g_themeMode = !wcscmp(b, L"dark") ? 1 : !wcscmp(b, L"light") ? 2 : 0;
    GetPrivateProfileStringW(L"View", L"Zoom", L"1", b, 64, SettingsPath().c_str());
    g_zoom = std::max(0.15, std::min(4.0, _wtof(b)));
    GetPrivateProfileStringW(L"View", L"PanX", L"0", b, 64, SettingsPath().c_str());
    g_panX = _wtof(b);
    GetPrivateProfileStringW(L"View", L"PanY", L"0", b, 64, SettingsPath().c_str());
    g_panY = _wtof(b);
}
static std::wstring IniGet(const wchar_t* sec, const wchar_t* key, const wchar_t* def = L"") {
    wchar_t b[512] = {};
    GetPrivateProfileStringW(sec, key, def, b, 512, SettingsPath().c_str());
    return b;
}
static void IniSet(const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(sec, key, v.empty() ? nullptr : v.c_str(), SettingsPath().c_str());
}
static void RestoreDefaultRender(bool announce);

static void SaveSettings() {
    WritePrivateProfileStringW(L"UI", L"Theme", g_themeMode == 1 ? L"dark" : g_themeMode == 2 ? L"light" : L"auto", SettingsPath().c_str());
    wchar_t vb[64];
    swprintf(vb, 64, L"%.3f", g_zoom); WritePrivateProfileStringW(L"View", L"Zoom", vb, SettingsPath().c_str());
    swprintf(vb, 64, L"%.1f", g_panX); WritePrivateProfileStringW(L"View", L"PanX", vb, SettingsPath().c_str());
    swprintf(vb, 64, L"%.1f", g_panY); WritePrivateProfileStringW(L"View", L"PanY", vb, SettingsPath().c_str());
}

// ======================================================================= UI: Hauptfenster

static HWND MkMain(const wchar_t* cls, const wchar_t* t, DWORD style, int id, HFONT f) {
    HWND c = CreateWindowExW(0, cls, t, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}
static void Place(HWND h, int x, int y, int w, int ht) { SetWindowPos(h, nullptr, S(x), S(y), S(w), S(ht), SWP_NOZORDER | SWP_NOACTIVATE); }

static void BuildToolbar() {
    g_btnSrc = MkMain(L"BARBtn", L"+ Quelle", WS_TABSTOP, IDC_BTN_SRC, g_fUI);
    g_btnDst = MkMain(L"BARBtn", L"+ Ausgang", WS_TABSTOP, IDC_BTN_DST, g_fUI);
    g_btnStream = MkMain(L"BARBtn", L"+ Stream-Ausgang", WS_TABSTOP, IDC_BTN_STREAM, g_fUI);
    g_btnStart = MkMain(L"BARBtn", L"Start", WS_TABSTOP, IDC_BTN_START, g_fBold);
    SendMessageW(g_btnStart, BTM_SETFLAGS, BTN_ACCENT, 0);
    g_btnVirt = MkMain(L"BARBtn", L"Virtuelles Gerät…", WS_TABSTOP, IDC_BTN_VIRT, g_fUI);
    g_btnRec = MkMain(L"BARBtn", L"●  Aufnahme-Box", WS_TABSTOP, IDC_BTN_REC, g_fUI);
    SendMessageW(g_btnRec, BTM_SETFLAGS, BTN_DRAGSRC, 0);
    g_btnSum = MkMain(L"BARBtn", L"∑  Summen-Modul", WS_TABSTOP, IDC_BTN_SUM, g_fUI);
    SendMessageW(g_btnSum, BTM_SETFLAGS, BTN_DRAGSRC, 0);
    g_btnWeb = MkMain(L"BARBtn", L"Web-Interface…", WS_TABSTOP, IDC_BTN_WEB, g_fUI);
    g_btnMore = MkMain(L"BARBtn", L"⋯", WS_TABSTOP, IDC_BTN_MORE, g_fUI);
    g_btnBg = MkMain(L"BARBtn", L"Hintergrund…", WS_TABSTOP, IDC_BTN_BG, g_fUI);
    g_btnLayout = MkMain(L"BARBtn", L"Layouts", WS_TABSTOP, IDC_BTN_LAYOUT, g_fUI);
    g_btnAbout = MkMain(L"BARBtn", L"Info", WS_TABSTOP, IDC_BTN_ABOUT, g_fUI);
    g_btnTheme = MkMain(L"BARBtn", L"Design", WS_TABSTOP, IDC_BTN_THEME, g_fUI);
    g_lblPad = MkMain(L"STATIC", L"Ausgabe-Puffer", SS_LEFT, 0, g_fSmall);
    g_lblBase = MkMain(L"STATIC", L"Safety-Puffer", SS_LEFT, 0, g_fSmall);
    g_lblPoll = MkMain(L"STATIC", L"Poll-Takt", SS_LEFT, 0, g_fSmall);
    g_lblDrv = MkMain(L"STATIC", L"Treiber-Puffer", SS_LEFT, 0, g_fSmall);
    for (HWND l : {g_lblPad, g_lblBase, g_lblPoll, g_lblDrv}) SetWindowLongPtrW(l, GWLP_USERDATA, 1);
    g_cmbPad = MkMain(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_CMB_PAD, g_fUI);
    g_cmbBase = MkMain(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_CMB_BASE, g_fUI);
    g_cmbPoll = MkMain(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_CMB_POLL, g_fUI);
    g_cmbDrv = MkMain(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_CMB_DRV, g_fUI);
    wchar_t b[32];
    auto fill = [&](HWND c, const int* v, int n, int cur) {
        int idx = 0;
        for (int i = 0; i < n; i++) {
            swprintf(b, 32, L"%d ms", v[i]);
            SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)b);
            if (v[i] == cur) idx = i;
        }
        SendMessageW(c, CB_SETCURSEL, idx, 0);
    };
    fill(g_cmbPad, PAD_VALS, 5, g_padMs.load());
    fill(g_cmbBase, BASE_VALS, 7, g_baseMs.load());
    fill(g_cmbPoll, POLL_VALS, 4, g_pollMs.load());
    fill(g_cmbDrv, DRV_VALS, 4, g_drvMs.load());
    g_tgSync = CreateWindowExW(0, L"BARToggle", L"Autopilot", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_TG_SYNC,
                               GetModuleHandleW(nullptr), nullptr);
    SendMessageW(g_tgSync, TGM_SET, g_autoSync ? 1 : 0, 0);
    g_btnSyncNow = MkMain(L"BARBtn", L"Einmessen…", WS_TABSTOP, IDC_BTN_SYNCNOW, g_fUI);
    SendMessageW(g_btnSyncNow, BTM_SETFLAGS, BTN_ACCENT, 0);
    g_btnRescan = MkMain(L"BARBtn", L"Geräte neu einlesen", WS_TABSTOP, IDC_BTN_RESCAN, g_fUI);
}

static void SyncToolbarFromSettings() {
    auto sel = [](HWND c, const int* v, int n, int cur) {
        for (int i = 0; i < n; i++) if (v[i] == cur) SendMessageW(c, CB_SETCURSEL, i, 0);
    };
    sel(g_cmbPad, PAD_VALS, 5, g_padMs.load());
    sel(g_cmbBase, BASE_VALS, 7, g_baseMs.load());
    sel(g_cmbPoll, POLL_VALS, 4, g_pollMs.load());
    sel(g_cmbDrv, DRV_VALS, 4, g_drvMs.load());
    SendMessageW(g_tgSync, TGM_SET, g_autoSync ? 1 : 0, 0);
}

static std::vector<HWND> g_tbHidden;
static void ArrangeOverflow(int rx) {   // Schmales Fenster: Schaltflaechen, die nicht mehr passen, wandern ins Menue "⋯"
    HWND cand[] = {g_btnVirt, g_btnSyncNow, g_btnRescan, g_btnRec, g_btnSum, g_btnWeb, g_btnBg}; HWND right[] = {g_btnLayout, g_btnTheme, g_btnAbout}; g_tbHidden.clear();
    auto rect = [&](HWND h) { RECT r; GetWindowRect(h, &r); MapWindowPoints(nullptr, g_hwnd, (POINT*)&r, 2); r.left = MulDiv(r.left, 96, g_dpi); r.right = MulDiv(r.right, 96, g_dpi); r.top = MulDiv(r.top, 96, g_dpi); return r; };
    int lim[2] = {rx - 14, rx - 14}; for (HWND h : right) lim[0] = std::min(lim[0], (int)rect(h).left - 8);
    auto over = [&](int slack) { bool any = false; for (HWND h : cand) { RECT r = rect(h); int row = r.top > 50 ? 1 : 0; if (r.right > lim[row] - slack) any = true; } return any; };
    int slack = over(0) ? 52 : 0; int lastRight = 0;
    for (HWND h : cand) { RECT r = rect(h); int row = r.top > 50 ? 1 : 0; bool hide = r.right > lim[row] - slack; ShowWindow(h, hide ? SW_HIDE : SW_SHOW); if (hide) g_tbHidden.push_back(h); else if (row == 0) lastRight = std::max(lastRight, (int)r.right); }
    if (!slack || g_tbHidden.empty()) { ShowWindow(g_btnMore, SW_HIDE); return; }
    HWND keep[] = {g_btnSrc, g_btnDst, g_btnStream, g_btnStart}; for (HWND h : keep) lastRight = std::max(lastRight, (int)rect(h).right);
    Place(g_btnMore, lastRight + 8, 12, 44, 32); ShowWindow(g_btnMore, SW_SHOW);
}
static void ShowMoreMenu() {
    HMENU m = CreatePopupMenu(); for (size_t i = 0; i < g_tbHidden.size(); i++) { wchar_t t[80] = {}; GetWindowTextW(g_tbHidden[i], t, 80); AppendMenuW(m, MF_STRING, 1000 + (UINT)i, t); }
    RECT r; GetWindowRect(g_btnMore, &r); int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, g_hwnd, nullptr); DestroyMenu(m);
    if (cmd >= 1000 && (size_t)(cmd - 1000) < g_tbHidden.size()) { HWND h = g_tbHidden[cmd - 1000]; PostMessageW(g_hwnd, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), (LPARAM)h); }
}
static void LayoutMain(int w, int h) {
    int cw = w, ch = h;
    int tb = S(TOOLBAR_H), iw = S(INSP_W), sh = S(STATUS_H);
    SetWindowPos(g_canvas, nullptr, 0, tb, cw - iw, ch - tb - sh, SWP_NOZORDER);
    SetWindowPos(g_insp, nullptr, cw - iw, tb, iw, ch - tb - sh, SWP_NOZORDER);
    Place(g_btnSrc, 14, 12, 104, 32);
    Place(g_btnDst, 124, 12, 112, 32);
    Place(g_btnStream, 242, 12, 150, 32);
    Place(g_btnStart, 410, 12, 120, 32);
    Place(g_btnVirt, 548, 12, 176, 32);
    Place(g_btnRec, 842, 68, 148, 30);
    Place(g_btnSum, 998, 68, 156, 30);
    Place(g_btnWeb, 1162, 68, 160, 30);
    Place(g_btnBg, 1330, 68, 150, 30);
    int rx = MulDiv(cw, 96, g_dpi);
    Place(g_btnAbout, rx - 14 - 64, 12, 64, 32);
    Place(g_btnTheme, rx - 14 - 64 - 8 - 96, 12, 96, 32);
    Place(g_btnLayout, rx - 14 - 64 - 8 - 96 - 8 - 104, 12, 104, 32);
    int x = 14, y = 56;
    Place(g_lblPad, x, y - 1, 100, 14); Place(g_cmbPad, x, y + 14, 92, 220); x += 100;
    Place(g_lblBase, x, y - 1, 100, 14); Place(g_cmbBase, x, y + 14, 92, 220); x += 100;
    Place(g_lblPoll, x, y - 1, 100, 14); Place(g_cmbPoll, x, y + 14, 92, 220); x += 100;
    Place(g_lblDrv, x, y - 1, 100, 14); Place(g_cmbDrv, x, y + 14, 92, 220); x += 116;
    Place(g_tgSync, x, y + 14, 110, 26); x += 122;
    Place(g_btnSyncNow, x, y + 12, 104, 30); x += 112;
    Place(g_btnRescan, x, y + 12, 160, 30);
    LayoutInspector();
    ArrangeOverflow(MulDiv(cw, 96, g_dpi));
}

static void ShowAddMenu(bool outputs) {
    HMENU m = CreatePopupMenu();
    if (outputs) {
        for (size_t i = 0; i < g_renderDevs.size(); i++) AppendMenuW(m, MF_STRING, 4000 + (UINT)i, g_renderDevs[i].name.c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 4900, L"●  Aufnahme-Box (WAV-Datei)");
        if (g_renderDevs.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 1, L"(keine Ausgabegeräte gefunden)");
    } else {
        for (size_t i = 0; i < g_renderDevs.size(); i++) AppendMenuW(m, MF_STRING, 3000 + (UINT)i, (L"Wiedergabe-Mitschnitt: " + g_renderDevs[i].name).c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < g_captureDevs.size(); i++) AppendMenuW(m, MF_STRING, 3500 + (UINT)i, (L"Eingang: " + g_captureDevs[i].name).c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 3900, L"Testgenerator: Klick (1 Hz)");
        AppendMenuW(m, MF_STRING, 3901, L"Testgenerator: Sinus 1 kHz");
        AppendMenuW(m, MF_STRING, 3902, L"Testgenerator: Rosa Rauschen");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 3950, L"AirPlay-Empfänger (iPhone / Mac → Router)");
        AppendMenuW(m, MF_STRING, 3960, L"∑  Summen-Modul (Mono → L/R, Kanäle zuordnen)");
        if (g_simDelayMs >= 0) AppendMenuW(m, MF_STRING, 3903, L"Test: Simuliertes Mikrofon (Debug)");
    }
    RECT r;
    GetWindowRect(outputs ? g_btnDst : g_btnSrc, &r);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom + 2, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    Node* n = nullptr;
    if (cmd >= 4000 && cmd < 4000 + (int)g_renderDevs.size()) n = AddNode(K_OUT, g_renderDevs[cmd - 4000].id, g_renderDevs[cmd - 4000].name, 0, 0);
    else if (cmd >= 3000 && cmd < 3000 + (int)g_renderDevs.size()) n = AddNode(K_LOOP, g_renderDevs[cmd - 3000].id, L"Mitschnitt: " + g_renderDevs[cmd - 3000].name, 0, 0);
    else if (cmd >= 3500 && cmd < 3500 + (int)g_captureDevs.size()) n = AddNode(K_CAPTURE, g_captureDevs[cmd - 3500].id, L"Eingang: " + g_captureDevs[cmd - 3500].name, 0, 0);
    else if (cmd == 3900) n = AddNode(K_GEN, L"gen:click", L"Test: Klick (1 Hz)", 0, 0);
    else if (cmd == 3901) n = AddNode(K_GEN, L"gen:sine", L"Test: Sinus 1 kHz", 0, 0);
    else if (cmd == 3902) n = AddNode(K_GEN, L"gen:pink", L"Test: Rosa Rauschen", 0, 0);
    else if (cmd == 3950) n = AddNode(K_AIRPLAY, L"airplay", L"AirPlay: Bens Audio Router", 0, 0);
    else if (cmd == 4900) n = AddNode(K_REC, L"rec", L"Aufnahme-Box", 0, 0);
    else if (cmd == 3960) n = AddNode(K_SUM, L"sum", L"Summe (Mono → Stereo)", 0, 0);
    else if (cmd == 3903) n = AddNode(K_GEN, L"gen:simmic", L"Test: Sim-Mikrofon", 0, 0);
    if (n) {
        PlaceNodeAuto(n, outputs);
        SelectNode(n, nullptr);
        SaveGraph();
    }
}

static void AddStreamNode(const SonosDev* dev) {
    int port = 8100;
    for (;;) {
        bool used = false;
        for (Node* o : g_nodes) if (o->st && o->st->port == port) used = true;
        if (!used) break;
        port++;
    }
    std::wstring nm = dev ? L"Stream: " + dev->name : L"Stream: Sonos";
    Node* n = AddNode(K_STREAM, L"stream:" + std::to_wstring(port), nm, 0, 0);
    n->st->port = port;
    if (dev) {
        n->st->ip = dev->ip;
        n->st->room = dev->name;
    }
    PlaceNodeAuto(n, true);
    SelectNode(n, nullptr);
    SaveGraph();
}

static void ShowStreamMenu() {
    HMENU m = CreatePopupMenu();
    std::vector<SonosDev> list;
    EnterCriticalSection(&g_sonosCs);
    list = g_sonos;
    LeaveCriticalSection(&g_sonosCs);
    for (size_t i = 0; i < list.size(); i++) AppendMenuW(m, MF_STRING, 5000 + (UINT)i, (L"Sonos: " + list[i].name + L"  (" + W8(list[i].ip) + L")").c_str());
    if (!list.empty()) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 5900, L"Leerer Stream-Ausgang (IP manuell eingeben)");
    AppendMenuW(m, MF_STRING, 5901, g_discovering ? L"Suche läuft …" : L"Sonos im Netzwerk suchen");
    RECT r;
    GetWindowRect(g_btnStream, &r);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom + 2, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 5000 && cmd < 5000 + (int)list.size()) AddStreamNode(&list[cmd - 5000]);
    else if (cmd == 5900) AddStreamNode(nullptr);
    else if (cmd == 5901) StartDiscovery();
}

static std::wstring DevName(const std::wstring& id) {
    for (auto& d : g_renderDevs) if (d.id == id) return d.name;
    return id;
}

static int PickFromMenu(const std::vector<std::wstring>& items, HWND anchor) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); i++) AppendMenuW(m, MF_STRING, 1 + (UINT)i, items[i].c_str());
    RECT r;
    GetWindowRect(anchor, &r);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom + 2, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    return cmd - 1;
}

static bool g_autoYes = false;  // nur Selbsttest: keine Rueckfragen

static void SetupVirtual() {
    EnumerateDevices();
    std::vector<size_t> virt;
    for (size_t i = 0; i < g_renderDevs.size(); i++) if (IsVirtualName(g_renderDevs[i].name)) virt.push_back(i);
    if (virt.empty()) {
        int r = g_autoYes ? IDNO : MessageBoxW(g_hwnd,
            L"Es wurde kein virtuelles Wiedergabegerät gefunden.\n\n"
            L"Windows erlaubt es Programmen nicht, selbst ein Audiogerät zu erzeugen – dafür ist ein signierter Treiber nötig.\n"
            L"Installiere einmalig das kostenlose VB-Audio HIFI-CABLE (bit-perfekt, bis 24 Bit / 384 kHz) oder VB-CABLE, starte den PC neu "
            L"und öffne diesen Assistenten dann erneut.\n\nDownload-Seite jetzt öffnen?",
            L"Virtuelles Gerät einrichten", MB_YESNO | MB_ICONINFORMATION);
        if (r == IDYES) ShellExecuteW(nullptr, L"open", L"https://vb-audio.com/Cable/", nullptr, nullptr, SW_SHOWNORMAL);
        g_msg = L"Kein virtuelles Wiedergabegerät installiert (z. B. VB-Audio HIFI-CABLE).";
        return;
    }
    size_t vi = virt[0];
    if (virt.size() > 1) {
        std::vector<std::wstring> names;
        for (size_t i : virt) names.push_back(g_renderDevs[i].name);
        int pick = g_autoYes ? 0 : PickFromMenu(names, g_btnVirt);
        if (pick < 0) return;
        vi = virt[(size_t)pick];
    }
    const DevInfo vdev = g_renderDevs[vi];

    // lokales Ausgabegeraet = bisheriges Windows-Standardgeraet (sofern nicht selbst virtuell)
    std::wstring defId = GetDefaultRenderId();
    std::wstring localId;
    for (auto& d : g_renderDevs) if (d.id == defId && !IsVirtualName(d.name)) localId = d.id;
    if (localId.empty()) {
        std::vector<std::wstring> names;
        std::vector<std::wstring> ids;
        for (auto& d : g_renderDevs) if (!IsVirtualName(d.name)) { names.push_back(L"Lokaler Ausgang: " + d.name); ids.push_back(d.id); }
        if (names.empty()) { g_msg = L"Kein physisches Ausgabegerät gefunden."; return; }
        int pick = g_autoYes ? 0 : PickFromMenu(names, g_btnVirt);
        if (pick < 0) return;
        localId = ids[(size_t)pick];
    }
    std::wstring ask = L"Der Assistent richtet Folgendes ein:\n\n"
                       L"• Quelle: Mitschnitt von \"" + vdev.name + L"\"\n"
                       L"• Lokaler Ausgang: \"" + DevName(localId) + L"\" (wird vom Autopilot passend zum langsamsten Ausgang verzögert)\n"
                       L"• alle vorhandenen Ausgänge (z. B. Sonos-Stream) werden mit der Quelle verbunden\n"
                       L"• Windows spielt ab jetzt standardmäßig auf \"" + vdev.name + L"\" ab\n\n"
                       L"Beim Beenden stellt der Router das bisherige Standardgerät wieder her. Fortfahren?";
    if (!g_autoYes && MessageBoxW(g_hwnd, ask.c_str(), L"Virtuelles Gerät einrichten", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    Node* src = nullptr;
    Node* loc = nullptr;
    for (Node* n : g_nodes) {
        if (n->kind == K_LOOP && n->devId == vdev.id) src = n;
        if (n->kind == K_OUT && n->devId == localId) loc = n;
    }
    if (!src) { src = AddNode(K_LOOP, vdev.id, L"Mitschnitt: " + vdev.name, 0, 0); PlaceNodeAuto(src, false); }
    if (!loc) { loc = AddNode(K_OUT, localId, DevName(localId), 0, 0); PlaceNodeAuto(loc, true); }
    for (Node* n : g_nodes) if (IsOutSink(n->kind)) TryConnect(src, n);
    g_autoSync = true;
    SendMessageW(g_tgSync, TGM_SET, 1, 0);

    if (IniGet(L"Virtual", L"OrigDefault").empty() && !defId.empty() && defId != vdev.id) IniSet(L"Virtual", L"OrigDefault", defId);
    HRESULT hr = SetDefaultRender(vdev.id);
    if (SUCCEEDED(hr)) {
        g_msg = L"Fertig: Windows spielt jetzt auf dem virtuellen Gerät, der Router verteilt und verzögert automatisch.";
    } else {
        g_msg = L"Verkabelt. Standardgerät bitte selbst auf das virtuelle Gerät stellen (Windows-Sound-Einstellungen geöffnet).";
        if (!g_autoYes) ShellExecuteW(nullptr, L"open", L"control.exe", L"mmsys.cpl,,0", nullptr, SW_SHOWNORMAL);
    }
    SelectNode(src, nullptr);
    SaveGraph();
}

static void RestoreDefaultRender(bool announce) {
    std::wstring orig = IniGet(L"Virtual", L"OrigDefault");
    if (orig.empty()) { if (announce) g_msg = L"Kein ursprüngliches Standardgerät gespeichert."; return; }
    HRESULT hr = SetDefaultRender(orig);
    if (SUCCEEDED(hr)) {
        IniSet(L"Virtual", L"OrigDefault", L"");
        if (announce) g_msg = L"Windows-Standardgerät wiederhergestellt.";
    } else if (announce) g_msg = L"Zurücksetzen nicht möglich – bitte in den Windows-Sound-Einstellungen wählen.";
}

static void ShowAbout() {
    std::wstring txt = std::wstring(APP_NAME L" " APP_VERSION L"\nAudiophiler Node-Router für Windows\n\n") + APP_COPYRIGHT + L"\n"
                       L"Software Publisher: " APP_PUBLISHER L"\n\n"
                       L"Open Source (MIT-Lizenz). Enthält: WASAPI-Routing, Sinc-Resampler, FLAC-24-Bit-Encoder,\n"
                       L"Sonos-Streaming (UPnP), AirPlay-Empfänger (RAOP), akustische Latenz-Einmessung.\n\n"
                       L"Webseite " APP_URL L" jetzt öffnen?";
    MSGBOXPARAMSW mb = {};
    mb.cbSize = sizeof(mb);
    mb.hwndOwner = g_hwnd;
    mb.hInstance = GetModuleHandleW(nullptr);
    mb.lpszText = txt.c_str();
    mb.lpszCaption = L"Über " APP_NAME;
    mb.dwStyle = MB_YESNO | MB_USERICON;
    mb.lpszIcon = MAKEINTRESOURCEW(1);
    if (MessageBoxIndirectW(&mb) == IDYES) ShellExecuteW(nullptr, L"open", APP_URL, nullptr, nullptr, SW_SHOWNORMAL);
}

static void ShowVirtualMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Assistent: virtuelles Gerät einrichten und verkabeln");
    AppendMenuW(m, MF_STRING, 2, L"Windows-Standardgerät zurücksetzen");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (IniGet(L"Virtual", L"AutoRestore", L"1") == L"1" ? MF_CHECKED : 0), 3, L"Beim Beenden Standardgerät automatisch zurücksetzen");
    AppendMenuW(m, MF_STRING, 4, L"Windows-Sound-Einstellungen öffnen");
    AppendMenuW(m, MF_STRING, 5, L"VB-Audio HIFI-CABLE / VB-CABLE herunterladen …");
    RECT r;
    GetWindowRect(g_btnVirt, &r);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom + 2, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 1) SetupVirtual();
    else if (cmd == 2) RestoreDefaultRender(true);
    else if (cmd == 3) IniSet(L"Virtual", L"AutoRestore", IniGet(L"Virtual", L"AutoRestore", L"1") == L"1" ? L"0" : L"1");
    else if (cmd == 4) ShellExecuteW(nullptr, L"open", L"control.exe", L"mmsys.cpl,,0", nullptr, SW_SHOWNORMAL);
    else if (cmd == 5) ShellExecuteW(nullptr, L"open", L"https://vb-audio.com/Cable/", nullptr, nullptr, SW_SHOWNORMAL);
}

static void ResetAfterLayoutChange() {
    SyncToolbarFromSettings();
    g_sel = nullptr;
    g_selConn = nullptr;
    UpdateInspector();
    SaveGraph();
}

static void ShowLayoutMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Layout speichern unter …");
    AppendMenuW(m, MF_STRING, 2, L"Layout laden …");
    AppendMenuW(m, MF_STRING, 3, L"Neues, leeres Layout");
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((LayoutDir() + L"\\*.bar").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do { files.push_back(fd.cFileName); } while (FindNextFileW(hf, &fd) && files.size() < 20);
        FindClose(hf);
    }
    if (!files.empty()) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < files.size(); i++) {
            std::wstring nm = files[i].substr(0, files[i].size() - 4);
            AppendMenuW(m, MF_STRING, 100 + (UINT)i, nm.c_str());
        }
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 4, L"Layout-Ordner öffnen");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 5, L"Ansicht: alles anzeigen (F)");
    AppendMenuW(m, MF_STRING, 6, L"Ansicht: 100 % (0)");
    RECT r;
    GetWindowRect(g_btnLayout, &r);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom + 2, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 0) return;
    if (cmd == 4) { ShellExecuteW(nullptr, L"open", LayoutDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); return; }
    if (cmd == 5) { FitView(); return; }
    if (cmd == 6) { g_zoom = 1.0; g_panX = g_panY = 0; return; }
    if (cmd == 3) {
        EnterCriticalSection(&g_cs);
        LeaveCriticalSection(&g_cs);
        ClearGraph();
        ResetAfterLayoutChange();
        g_msg = L"Neues leeres Layout.";
        return;
    }
    wchar_t path[MAX_PATH] = {};
    if (cmd == 1 || cmd == 2) {
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_hwnd;
        ofn.lpstrFilter = L"Bens Router Layout (*.bar)\0*.bar\0Alle Dateien\0*.*\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH;
        std::wstring dir = LayoutDir();
        ofn.lpstrInitialDir = dir.c_str();
        ofn.lpstrDefExt = L"bar";
        if (cmd == 1) {
            ofn.lpstrTitle = L"Layout speichern";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (!GetSaveFileNameW(&ofn)) return;
            g_msg = SaveGraphTo(path) ? L"Layout gespeichert." : L"Speichern fehlgeschlagen.";
        } else {
            ofn.lpstrTitle = L"Layout laden";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (!GetOpenFileNameW(&ofn)) return;
            g_msg = LoadGraphFrom(path, true) ? L"Layout geladen." : L"Laden fehlgeschlagen.";
            ResetAfterLayoutChange();
        }
        return;
    }
    if (cmd >= 100 && cmd < 100 + (int)files.size()) {
        g_msg = LoadGraphFrom(LayoutDir() + L"\\" + files[cmd - 100], true) ? L"Layout geladen: " + files[cmd - 100] : L"Laden fehlgeschlagen.";
        ResetAfterLayoutChange();
    }
}

static void ShowThemeMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_themeMode == 0 ? MF_CHECKED : 0), 1, L"Automatisch (wie Windows)");
    AppendMenuW(m, MF_STRING | (g_themeMode == 1 ? MF_CHECKED : 0), 2, L"Dunkel");
    AppendMenuW(m, MF_STRING | (g_themeMode == 2 ? MF_CHECKED : 0), 3, L"Hell");
    RECT r;
    GetWindowRect(g_btnTheme, &r);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom + 2, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 1 && cmd <= 3) {
        g_themeMode = cmd - 1;
        SaveSettings();
        ApplyThemeNow();
    }
}

static void TrayUpdate();
static void SetStartButton() {
    TrayUpdate(); SetWindowTextW(g_btnStart, g_running ? L"Stopp" : L"Start"); SendMessageW(g_btnStart, BTM_SETFLAGS, g_running ? BTN_DANGER : BTN_ACCENT, 0); }

// ======================================================================= Web-Interface (Handy-Bedienung, nur HTTPS) – Protokoll: docs/WEB-API.md
static const UINT WM_WEBCMD = WM_APP + 40;
static std::atomic<bool> g_webQuit{false};
static int g_webPort = 8443;
struct WebReq { int kind = 0; web::JObj cmd; std::string err, out; bool ok = false; };   // kind 0 = Befehl, 1 = Zustand abfragen (laeuft immer im UI-Thread)

static const char* WebKind(int k) { switch (k) { case K_LOOP: return "loop"; case K_CAPTURE: return "cap"; case K_GEN: return "gen"; case K_AIRPLAY: return "air"; case K_OUT: return "out"; case K_STREAM: return "stream"; case K_REC: return "rec"; default: return "sum"; } }
static std::string WebF(double v) { char b[40]; snprintf(b, sizeof b, "%.1f", v); return b; }
static std::string WebB(bool v) { return v ? "true" : "false"; }
static std::string WebStatus(Node* n) {
    std::wstring s;
    if (n->kind == K_STREAM) s = StreamStatusLine(n); else if (n->kind == K_AIRPLAY) s = ApStatusLine(n); else if (n->kind == K_REC) s = RecStatusLine(n); else if (n->kind == K_SUM) s = SumStatusLine(n);
    else if (n->rate > 0 && g_running) s = std::to_wstring(n->rate) + L" Hz";
    return U8(s);
}

static void HandleCommand(int id, int code, HWND src);
static void SetupVirtual();
static void AddStreamNode(const SonosDev* dev);
static void UpdateGroupList();
static void TrayUpdate();
static std::string WebProps(Node* n) {   // kindspezifische Einstellungen fuer die Fernbedienung
    char b[520];
    if (n->kind == K_GEN) { snprintf(b, sizeof b, ",\"p\":{\"gen\":%d}", std::max(0, std::min(2, n->genType))); return b; }
    if (n->kind == K_REC && n->rc) { RecCtx* rc = n->rc; int f = rc->flac ? (rc->bits == 16 ? 2 : 1) : (rc->bits == 16 ? 3 : rc->bits == 32 ? 4 : 0); snprintf(b, sizeof b, ",\"p\":{\"fmt\":%d,\"rate\":%d,\"dir\":", f, (int)rc->rate); return b + web::JStr(U8(rc->dir)) + "}"; }
    if (n->kind == K_STREAM && n->st) { StreamCtx* s = n->st;
        snprintf(b, sizeof b, ",\"p\":{\"port\":%d,\"scheme\":%d,\"rate\":%d,\"burst\":%d,\"fmt\":%d,\"volCtl\":%s,\"dither\":%s,\"autoLat\":%s,\"vol\":%d,\"play\":%s,\"ctl\":%d,\"ip\":", (int)s->port, (int)s->scheme, (int)s->rate, (int)s->burstMs, (int)s->format == 1 ? 0 : 1, WebB((bool)s->volCtl).c_str(), WebB((bool)s->dither).c_str(), WebB((bool)s->autoLat).c_str(), (int)s->sonosVol, WebB((bool)s->playWanted).c_str(), (int)s->ctl);
        std::string o = b + web::JStr(s->ip) + ",\"room\":" + web::JStr(U8(s->room)) + ",\"group\":["; EnterCriticalSection(&s->cs);
        for (size_t i = 0; i < s->groupIps.size(); i++) o += std::string(i ? "," : "") + "{\"ip\":" + web::JStr(s->groupIps[i]) + ",\"name\":" + web::JStr(U8(i < s->groupNames.size() ? s->groupNames[i] : std::wstring())) + "}";
        LeaveCriticalSection(&s->cs); return o + "]}"; }
    if (n->kind == K_AIRPLAY && n->ap) { AirplayCtx* a = n->ap; snprintf(b, sizeof b, ",\"p\":{\"port\":%d,\"buf\":%d,\"latAuto\":%s,\"latMs\":%d,\"useVol\":%s,\"name\":", (int)a->port, (int)a->bufferMs, WebB((bool)a->latAuto).c_str(), (int)a->latManualMs, WebB((bool)a->useVol).c_str()); return b + web::JStr(U8(a->name)) + "}"; }
    return "";
}
static std::string WebGlobal() {
    std::string o = "\"engine\":{\"running\":" + WebB(g_running) + "},\"global\":{\"pad\":" + std::to_string((int)g_padMs) + ",\"base\":" + std::to_string((int)g_baseMs) + ",\"poll\":" + std::to_string((int)g_pollMs) + ",\"drv\":" + std::to_string((int)g_drvMs) + ",\"opts\":{\"pad\":[10,20,30,50,100],\"base\":[10,20,30,50,100,200,500],\"poll\":[1,2,3,5],\"drv\":[20,50,100,200]}},\"sonosFound\":[";
    EnterCriticalSection(&g_sonosCs); for (size_t i = 0; i < g_sonos.size(); i++) o += std::string(i ? "," : "") + "{\"ip\":" + web::JStr(g_sonos[i].ip) + ",\"name\":" + web::JStr(U8(g_sonos[i].name)) + ",\"model\":" + web::JStr(U8(g_sonos[i].model)) + "}"; LeaveCriticalSection(&g_sonosCs);
    return o + "],\"msg\":" + web::JStr(U8(g_msg));
}
static bool WebSonosFind(const std::string& ip, SonosDev& d) { bool ok = false; EnterCriticalSection(&g_sonosCs); for (auto& x : g_sonos) if (x.ip == ip) { d = x; ok = true; break; } LeaveCriticalSection(&g_sonosCs); return ok; }
static std::string WebHw() {
    auto lst = [&](const std::vector<DevInfo>& v) { std::string o = "["; for (size_t i = 0; i < v.size(); i++) o += std::string(i ? "," : "") + "{\"id\":" + web::JStr(U8(v[i].id)) + ",\"name\":" + web::JStr(U8(v[i].name)) + "}"; return o + "]"; };
    return "\"hw\":{\"cap\":" + lst(g_captureDevs) + ",\"out\":" + lst(g_renderDevs) + "}";
}
static Node* WebRemoteMic() { for (Node* n : g_nodes) if (n->remote) return n; return nullptr; }
static void WebMicChunk(const std::string& d) {   // Server-Thread: [1][0][0][0][rate u32][start u64][int16 mono ...]
    if (d.size() < 18 || (uint8_t)d[0] != 1) return; uint32_t rate; uint64_t start; memcpy(&rate, d.data() + 4, 4); memcpy(&start, d.data() + 8, 8);
    size_t ns = (d.size() - 16) / 2; if (ns == 0 || ns > 8192 || rate < 8000 || rate > 96000) return;
    EnterCriticalSection(&g_cs); Node* n = nullptr; for (Node* x : g_nodes) if (x->remote && x->state.load() == 1) { n = x; break; }
    if (n && (int)rate == n->rate) {
        double now = NowSec(), st = (double)start / rate, off = now - st;
        n->remOff = std::min(n->remOff + 0.00005, off);   // Minimum-Filter: kleinste Laufzeit = bester Zeit-Schaetzwert, langsame Entspannung (Taktdrift)
        if (n->remNext != 0 && start != n->remNext) n->tsN = 0;   // Luecke/Sprung: Zeitachse neu anlegen
        TsAdd(n, n->sb.head.load(), st + n->remOff); n->remNext = start + ns; n->remLast = now;
        const int16_t* p = (const int16_t*)(d.data() + 16); for (size_t i = 0; i < ns; i++) { float v = p[i] / 32768.0f; n->sb.push(v, v); }
    }
    LeaveCriticalSection(&g_cs);
}
static std::string WebScenes() {
    std::string o; WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((LayoutDir() + L"\\*.bar").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { std::vector<std::wstring> v; do { std::wstring f = fd.cFileName; v.push_back(f.substr(0, f.size() - 4)); } while (FindNextFileW(h, &fd)); FindClose(h); std::sort(v.begin(), v.end());
        for (size_t i = 0; i < v.size(); i++) o += std::string(i ? "," : "") + web::JStr(U8(v[i])); }
    return o;
}
static void WebBuildState(std::string& o) {   // UI-Thread: Node-/Kabel-Listen werden nur hier veraendert
    o = std::string("\"running\":") + WebB(g_running) + ",\"autopilot\":" + WebB(g_autoSync) + ",\"nodes\":[";
    bool f = true;
    for (Node* n : g_nodes) {
        if (!f) o += ","; f = false;
        o += "{\"id\":" + std::to_string(n->id) + ",\"kind\":\"" + WebKind(n->kind) + "\",\"name\":" + web::JStr(U8(n->name)) + ",\"x\":" + WebF(n->x) + ",\"y\":" + WebF(n->y) + ",\"ports\":\"" + (n->kind == K_SUM ? "both" : IsSink(n->kind) ? "in" : "out") + "\",\"gain\":" + WebF(n->gainDb) +
             ",\"mute\":" + WebB(n->mute.load()) + ",\"delay\":" + WebF(n->delayMs.load() + n->trimMs.load()) + ",\"dly\":" + WebF(n->delayMs.load()) + ",\"trim\":" + WebF(n->trimMs.load()) + ",\"status\":" + web::JStr(WebStatus(n)) + ",\"sumMono\":" + WebB(n->sumMono.load());
        if (IsOutSink(n->kind)) { SyncInfo si = GetSync(n); char lb[160]; snprintf(lb, sizeof lb, ",\"ext\":%.1f,\"lat\":{\"total\":%.1f,\"err\":%.1f,\"src\":", (double)n->extMs, si.total, si.err); o += lb + web::JStr(U8(SyncSrcName(si.src))) + "}"; }
        o += WebProps(n);
        if (n->kind == K_STREAM && n->st && ZoneCount(n) > 0) {
            StreamCtx* sc = n->st; o += ",\"zones\":[";
            for (int z = 0; z < ZoneCount(n); z++) {
                std::wstring nm = z == 0 ? (!sc->room.empty() ? CleanText(sc->room) : W8(sc->ip)) : ((size_t)z - 1 < sc->groupNames.size() && !sc->groupNames[z - 1].empty() ? CleanText(sc->groupNames[z - 1]) : ((size_t)z - 1 < sc->groupIps.size() ? W8(sc->groupIps[z - 1]) : L"Zone"));
                int v = ZoneVol(sc, z); o += std::string(z ? "," : "") + "{\"name\":" + web::JStr(U8(nm)) + ",\"vol\":" + WebF(std::max(0, v) / 100.0).substr(0, 4) + "}";
            }
            o += "]";
        }
        if (n->kind == K_REC && n->rc) { RecCtx* rc = n->rc; char b[160]; snprintf(b, sizeof b, ",\"rec\":{\"on\":%s,\"sec\":%.1f,\"bytes\":%.0f,\"fmt\":", WebB(rc->rec.active()).c_str(), (double)rc->rec.frames() / std::max(1, rc->rate), (double)rc->rec.bytes()); o += b + web::JStr(U8(std::wstring(RecFmtName(rc->bits, rc->flac)) + L" · " + std::to_wstring(rc->rate) + L" Hz")) + "}"; }
        o += "}";
    }
    o += "],\"conns\":["; f = true;
    for (Conn* c : g_conns) { if (!f) o += ","; f = false; o += "{\"id\":" + std::to_string(c->id) + ",\"src\":" + std::to_string(c->src->id) + ",\"dst\":" + std::to_string(c->dst->id) + ",\"srcCh\":" + std::to_string(c->srcCh) + ",\"dstCh\":" + std::to_string(c->dstCh) + ",\"lag\":" + WebF(LagFor(c->src, c->dst)) + "}"; }
    { Node* m = WebRemoteMic(); bool live = m && m->state.load() == 1 && NowSec() - m->remLast < 2.0;
      o += "],\"scenes\":[" + WebScenes() + "]," + WebHw() + ",\"mic\":{\"on\":" + WebB(live) + ",\"exists\":" + WebB(m != nullptr) + "},\"cal\":{\"busy\":" + WebB(g_calibActive.load()) + ",\"msg\":" + web::JStr(U8(g_calibMsg)) + "}," + WebGlobal(); }
}
static Node* WebNode(const web::JObj& c, const char* key) { double id; if (!web::GetNum(c, key, id)) return nullptr; for (Node* n : g_nodes) if (n->id == (int)id && id == (int)id) return n; return nullptr; }
static std::wstring WebSceneFile(const std::string& name, bool must) {   // Layout-Name bereinigen (kein Pfad, keine Sonderzeichen)
    std::wstring w; for (wchar_t ch : W8(name)) { if (iswalnum(ch) || ch == L' ' || ch == L'.' || ch == L'_' || ch == L'-') w += ch; if (w.size() >= 40) break; }
    while (!w.empty() && (w.back() == L' ' || w.back() == L'.')) w.pop_back(); if (w.empty() || w[0] == L'.') return L"";
    std::wstring p = LayoutDir() + L"\\" + w + L".bar"; if (must && GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return L""; return p;
}
static void WebSaveLater() { SetTimer(g_hwnd, 77, 1000, nullptr); }   // Autospeichern nach 1 s Ruhe (Fader senden viele Werte)
static bool WebExecUi(WebReq& r) {   // laeuft im UI-Thread; prueft ALLE Regeln (der Client wird nie vorausgesetzt)
    if (r.kind == 1) { WebBuildState(r.out); return true; }
    const web::JObj& c = r.cmd; std::string op; web::GetStr(c, "op", op); auto bad = [&](const char* e) { r.err = e; return false; };
    if (op == "gain") { Node* n = WebNode(c, "node"); double db; if (!n || !web::GetNum(c, "db", db) || db < -60 || db > 0) return bad("Ungültig"); n->gainDb = (float)db; n->gainLin = std::pow(10.0f, n->gainDb / 20.0f); if (g_sel == n) UpdateInspector(); InvalidateRect(g_canvas, nullptr, FALSE); WebSaveLater(); return true; }
    if (op == "mute") { Node* n = WebNode(c, "node"); bool on; if (!n || !web::GetBool(c, "on", on)) return bad("Ungültig"); n->mute = on; if (g_sel == n) UpdateInspector(); InvalidateRect(g_canvas, nullptr, FALSE); WebSaveLater(); return true; }
    if (op == "autopilot") { bool on; if (!web::GetBool(c, "on", on)) return bad("Ungültig"); g_autoSync = on; SendMessageW(g_tgSync, TGM_SET, on ? 1 : 0, 0); if (on) RecomputeSync(); UpdateInspector(); InvalidateRect(g_canvas, nullptr, FALSE); SaveGraph(); return true; }
    if (op == "rec") { Node* n = WebNode(c, "node"); bool on; if (!n || n->kind != K_REC || !n->rc || !web::GetBool(c, "on", on)) return bad("Keine Aufnahme-Box");
        if (on) { bool src = false; for (Conn* k : g_conns) if (k->dst == n) src = true; if (!src) return bad("Erst Quellen verbinden"); if (!n->rc->rec.active() && !RecStart(n)) { r.err = U8(n->rc->err.empty() ? L"Start nicht möglich" : n->rc->err); return false; } }
        else RecStop(n); InvalidateRect(g_canvas, nullptr, FALSE); if (g_sel == n) UpdateInspector(); return true; }
    if (op == "connect") { Node* a = WebNode(c, "src"); Node* b = WebNode(c, "dst"); if (!a || !b) return bad("Node fehlt"); if (a == b) return bad("Nicht mit sich selbst"); if (IsSink(a->kind)) return bad("Quelle hat keinen Ausgang"); if (!(IsSink(b->kind) || b->kind == K_SUM)) return bad("Ziel hat keinen Eingang");
        if (a->kind == K_SUM && b->kind == K_SUM) return bad("Summen-Module lassen sich nicht hintereinander schalten"); if (FindConn(a, b)) return bad("Schon verbunden"); if (!TryConnect(a, b)) return bad("Verbindung abgelehnt"); InvalidateRect(g_canvas, nullptr, FALSE); return true; }
    if (op == "disconnect") { double id; if (!web::GetNum(c, "conn", id)) return bad("Ungültig"); for (Conn* k : g_conns) if (k->id == (int)id) { RemoveConn(k); SaveGraph(); InvalidateRect(g_canvas, nullptr, FALSE); return true; } return bad("Verbindung fehlt"); }
    if (op == "route") { double id, sc, dc; if (!web::GetNum(c, "conn", id) || !web::GetNum(c, "srcCh", sc) || !web::GetNum(c, "dstCh", dc) || sc < 0 || sc > 3 || dc < 0 || dc > 3 || sc != (int)sc || dc != (int)dc) return bad("Ungültig");
        for (Conn* k : g_conns) if (k->id == (int)id) { if (k->dst->kind != K_SUM) return bad("Kanalzuordnung nur am Summen-Modul"); k->srcCh = (int)sc; k->dstCh = (int)dc; SaveGraph(); InvalidateRect(g_canvas, nullptr, FALSE); return true; } return bad("Verbindung fehlt"); }
    if (op == "sumMono") { Node* n = WebNode(c, "node"); bool on; if (!n || n->kind != K_SUM || !web::GetBool(c, "on", on)) return bad("Kein Summen-Modul"); n->sumMono = on; SaveGraph(); InvalidateRect(g_canvas, nullptr, FALSE); return true; }
    if (op == "zoneVol") { Node* n = WebNode(c, "node"); double z, v; if (!n || n->kind != K_STREAM || !n->st || !web::GetNum(c, "zone", z) || !web::GetNum(c, "vol", v) || v < 0 || v > 1 || z < 0 || z != (int)z || (int)z >= ZoneCount(n)) return bad("Ungültig"); SetZoneWant(n->st, (int)z, (int)std::lround(v * 100)); InvalidateRect(g_canvas, nullptr, FALSE); WebSaveLater(); return true; }
    if (op == "scene") { std::string nm; if (!web::GetStr(c, "name", nm)) return bad("Ungültig"); std::wstring p = WebSceneFile(nm, true); if (p.empty()) return bad("Layout fehlt"); if (!LoadGraphFrom(p, true)) return bad("Laden fehlgeschlagen"); g_sel = nullptr; UpdateInspector(); InvalidateRect(g_canvas, nullptr, FALSE); return true; }
    if (op == "sceneSave") { std::string nm; if (!web::GetStr(c, "name", nm)) return bad("Ungültig"); std::wstring p = WebSceneFile(nm, false); if (p.empty()) return bad("Name fehlt"); return SaveGraphTo(p) ? true : bad("Speichern fehlgeschlagen"); }
    if (op == "trim") { Node* n = WebNode(c, "node"); double ms; if (!n || !IsOutSink(n->kind) || !web::GetNum(c, "ms", ms) || ms < -500 || ms > 500) return bad("Ungültig"); n->trimMs = std::floor(ms * 10.0 + 0.5) / 10.0; if (g_sel == n) UpdateInspector(); InvalidateRect(g_canvas, nullptr, FALSE); WebSaveLater(); return true; }
    if (op == "delay") { Node* n = WebNode(c, "node"); double ms; if (!n || n->kind == K_REC || n->kind == K_SUM || !web::GetNum(c, "ms", ms) || ms < 0 || ms > 2000) return bad("Ungültig"); if (IsOutSink(n->kind) && g_autoSync) return bad("Autopilot ist an – bitte Feinkorrektur (Trim) nutzen");
        n->delayMs = std::floor(ms * 10.0 + 0.5) / 10.0; if (g_sel == n) UpdateInspector(); InvalidateRect(g_canvas, nullptr, FALSE); WebSaveLater(); return true; }
    if (op == "addNode") { if (g_nodes.size() >= 40) return bad("Zu viele Nodes"); std::string kd, dev; if (!web::GetStr(c, "kind", kd)) return bad("Ungültig"); web::GetStr(c, "dev", dev); Node* n = nullptr; std::wstring wd = W8(dev);
        if (kd == "cap") { for (auto& d : g_captureDevs) if (d.id == wd) n = AddNode(K_CAPTURE, d.id, L"Eingang: " + d.name, 0, 0); } else if (kd == "loop") { for (auto& d : g_renderDevs) if (d.id == wd) n = AddNode(K_LOOP, d.id, L"Mitschnitt: " + d.name, 0, 0); }
        else if (kd == "out") { for (auto& d : g_renderDevs) if (d.id == wd) n = AddNode(K_OUT, d.id, d.name, 0, 0); }
        else if (kd == "gen") { if (dev == "click") n = AddNode(K_GEN, L"gen:click", L"Test: Klick (1 Hz)", 0, 0); else if (dev == "sine") n = AddNode(K_GEN, L"gen:sine", L"Test: Sinus 1 kHz", 0, 0); else if (dev == "pink") n = AddNode(K_GEN, L"gen:pink", L"Test: Rosa Rauschen", 0, 0); }
        else if (kd == "air") n = AddNode(K_AIRPLAY, L"airplay", L"AirPlay: Bens Audio Router", 0, 0); else if (kd == "rec") n = AddNode(K_REC, L"rec", L"Aufnahme-Box", 0, 0); else if (kd == "sum") n = AddNode(K_SUM, L"sum", L"Summe (Mono → Stereo)", 0, 0);
        if (!n) return bad("Gerät nicht gefunden"); PlaceNodeAuto(n, IsSink(n->kind)); SaveGraph(); InvalidateRect(g_canvas, nullptr, FALSE); return true; }
    if (op == "removeNode") { Node* n = WebNode(c, "node"); if (!n) return bad("Node fehlt"); if (g_sel == n) g_sel = nullptr; RemoveNode(n); UpdateInspector(); SaveGraph(); InvalidateRect(g_canvas, nullptr, FALSE); return true; }
    if (op == "micStart") { double rate; if (!web::GetNum(c, "rate", rate) || rate < 8000 || rate > 96000 || rate != (int)rate) return bad("Ungültig"); if (!g_running) return bad("Erst „Start“ drücken");
        Node* m = WebRemoteMic(); if (!m) { m = AddNode(K_CAPTURE, L"remote:phone", L"Handy-Mikrofon", 0, 0); PlaceNodeAuto(m, false); } m->remRate = (int)rate; ReopenNode(m); InvalidateRect(g_canvas, nullptr, FALSE); return true; }
    if (op == "micStop") { Node* m = WebRemoteMic(); if (m) { if (g_sel == m) g_sel = nullptr; RemoveNode(m); UpdateInspector(); SaveGraph(); InvalidateRect(g_canvas, nullptr, FALSE); } return true; }
    if (op == "calibrate") { if (!g_running) return bad("Erst „Start“ drücken"); if (g_calibActive.load()) return bad("Einmessung läuft bereits"); Node* m = WebRemoteMic(); if (!m || NowSec() - m->remLast > 2.0) return bad("Handy-Mikrofon liefert keine Daten"); StartCalibration(); return true; }
    auto closeRun = [&](Node* x) { if (g_running) { EnterCriticalSection(&g_cs); CloseNode(x); LeaveCriticalSection(&g_cs); } };
    auto isIp = [&](const std::string& t) { unsigned a, b2, c2, d2; char x; return sscanf(t.c_str(), "%u.%u.%u.%u%c", &a, &b2, &c2, &d2, &x) == 4 && a < 256 && b2 < 256 && c2 < 256 && d2 < 256; };
    if (op == "engine") { bool on; if (!web::GetBool(c, "on", on)) return bad("Ungültig"); if (on != g_running) HandleCommand(IDC_BTN_START, BN_CLICKED, nullptr); return true; }
    if (op == "act") { std::string a; if (!web::GetStr(c, "name", a)) return bad("Ungültig");
        if (a == "rescan") { HandleCommand(IDC_BTN_RESCAN, BN_CLICKED, nullptr); return true; }
        if (a == "sonosSearch") { StartDiscovery(); g_msg = L"Suche nach Sonos-Geräten …"; return true; }
        if (a == "calibrateLocal") { if (!g_running) return bad("Erst „Start“ drücken"); if (g_calibActive.load()) return bad("Einmessung läuft bereits"); if (!PickMic()) return bad("Kein Mikrofon – erst einen Eingang (Mikrofon) hinzufügen"); StartCalibration(); return true; }
        if (a == "vdevSetup") { SetupVirtual(); return true; } if (a == "vdevRestore") { RestoreDefaultRender(true); return true; }
        return bad("Unbekannte Aktion"); }
    if (op == "global") { std::string k; double v; if (!web::GetStr(c, "key", k) || !web::GetNum(c, "value", v) || v != (int)v) return bad("Ungültig"); int iv = (int)v;
        auto pick = [&](const int* a, int cnt, HWND cmb, int& dst) { for (int i = 0; i < cnt; i++) if (a[i] == iv) { dst = a[i]; SendMessageW(cmb, CB_SETCURSEL, i, 0); SaveGraph(); return true; } return false; }; int tmp = 0;
        if (k == "pad") { if (!pick(PAD_VALS, 5, g_cmbPad, tmp)) return bad("Ungültiger Wert"); g_padMs = tmp; return true; } if (k == "base") { if (!pick(BASE_VALS, 7, g_cmbBase, tmp)) return bad("Ungültiger Wert"); g_baseMs = tmp; return true; }
        if (k == "poll") { if (!pick(POLL_VALS, 4, g_cmbPoll, tmp)) return bad("Ungültiger Wert"); g_pollMs = tmp; return true; } if (k == "drv") { if (!pick(DRV_VALS, 4, g_cmbDrv, tmp)) return bad("Ungültiger Wert"); g_drvMs = tmp; g_msg = L"Treiber-Puffer greift beim nächsten Start bzw. „Neu verbinden“."; return true; }
        return bad("Unbekannte Einstellung"); }
    if (op == "reconnect") { Node* n = WebNode(c, "node"); if (!n) return bad("Node fehlt"); ReopenNode(n); return true; }
    if (op == "move") { Node* n = WebNode(c, "node"); double x, y; if (!n || !web::GetNum(c, "x", x) || !web::GetNum(c, "y", y) || x < -3000 || x > 9000 || y < -3000 || y > 9000) return bad("Ungültig"); n->x = (float)x; n->y = (float)y; InvalidateRect(g_canvas, nullptr, FALSE); WebSaveLater(); return true; }
    if (op == "sceneDelete") { std::string nm; if (!web::GetStr(c, "name", nm)) return bad("Ungültig"); std::wstring p = WebSceneFile(nm, true); if (p.empty()) return bad("Layout fehlt"); return DeleteFileW(p.c_str()) ? true : bad("Löschen fehlgeschlagen"); }
    if (op == "addStream") { if (g_nodes.size() >= 40) return bad("Zu viele Nodes"); std::string ip; SonosDev d; if (web::GetStr(c, "ip", ip) && !ip.empty()) { if (!WebSonosFind(ip, d)) return bad("Zone nicht gefunden – erst suchen"); AddStreamNode(&d); } else AddStreamNode(nullptr); InvalidateRect(g_canvas, nullptr, FALSE); return true; }
    if (op == "sonosPick" || op == "groupAdd") { Node* n = WebNode(c, "node"); std::string ip; SonosDev d; if (!n || n->kind != K_STREAM || !n->st || !web::GetStr(c, "ip", ip)) return bad("Ungültig"); if (!WebSonosFind(ip, d)) return bad("Zone nicht gefunden – erst suchen");
        if (op == "sonosPick") { closeRun(n); n->st->ip = d.ip; n->st->room = d.name; n->name = L"Stream: " + d.name; UpdateInspector(); SaveGraph(); return true; }
        if (d.ip == n->st->ip) return bad("Das ist schon die Haupt-Zone (Koordinator)");
        EnterCriticalSection(&n->st->cs); if (std::find(n->st->groupIps.begin(), n->st->groupIps.end(), d.ip) == n->st->groupIps.end()) { n->st->groupIps.push_back(d.ip); n->st->groupNames.push_back(d.name); } LeaveCriticalSection(&n->st->cs);
        n->st->groupDirty = true; g_msg = L"Zone zur Gruppe hinzugefügt – Sonos synchronisiert sie selbst."; UpdateGroupList(); SaveGraph(); return true; }
    if (op == "groupClear") { Node* n = WebNode(c, "node"); if (!n || n->kind != K_STREAM || !n->st) return bad("Ungültig"); EnterCriticalSection(&n->st->cs); n->st->groupIps.clear(); n->st->groupNames.clear(); n->st->groupVol.clear(); n->st->groupWant.clear(); LeaveCriticalSection(&n->st->cs); n->st->groupDirty = true; UpdateGroupList(); SaveGraph(); return true; }
    if (op == "setProp") { Node* n = WebNode(c, "node"); std::string key, sv; double v = 0; bool b = false; if (!n || !web::GetStr(c, "key", key)) return bad("Ungültig");
        bool hasN = web::GetNum(c, "value", v), hasB = web::GetBool(c, "value", b), hasS = web::GetStr(c, "value", sv); int iv = (int)v; bool isInt = hasN && v == iv;
        if (key == "ext") { if (!IsOutSink(n->kind) || !hasN || v < 0 || v > 10000) return bad("Ungültig"); n->extMs = v; if (g_sel == n) UpdateInspector(); WebSaveLater(); return true; }
        if (n->kind == K_GEN && key == "gen") { if (!isInt || iv < 0 || iv > 2) return bad("Ungültig"); n->genType = iv; const wchar_t* ids[] = {L"gen:click", L"gen:sine", L"gen:pink"}; const wchar_t* nm[] = {L"Test: Klick (1 Hz)", L"Test: Sinus 1 kHz", L"Test: Rosa Rauschen"}; n->devId = ids[iv]; n->name = nm[iv]; UpdateInspector(); SaveGraph(); return true; }
        if (n->kind == K_REC && n->rc) { RecCtx* rc = n->rc; if (rc->rec.active() && key != "dir") return bad("Während der Aufnahme nicht änderbar");
            if (key == "fmt") { if (!isInt || iv < 0 || iv > 4) return bad("Ungültig"); rc->flac = (iv == 1 || iv == 2); rc->bits = (iv == 2 || iv == 3) ? 16 : iv == 4 ? 32 : 24; if (g_sel == n) UpdateInspector(); SaveGraph(); return true; }
            if (key == "rate") { static const int R[] = {44100, 48000, 88200, 96000}; bool ok = false; for (int r : R) if (isInt && r == iv) ok = true; if (!ok) return bad("Ungültig"); rc->rate = iv; n->rate = iv; ReopenNode(n); SaveGraph(); return true; }
            if (key == "dir") { std::wstring p = W8(sv); DWORD at = GetFileAttributesW(p.c_str()); if (!hasS || p.empty() || p.size() > 250 || at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) return bad("Ordner existiert nicht"); rc->dir = p; if (g_sel == n) UpdateInspector(); SaveGraph(); return true; } }
        if (n->kind == K_STREAM && n->st) { StreamCtx* s = n->st;
            if (key == "ip") { if (!hasS || !isIp(sv)) return bad("Ungültige IP-Adresse"); if (sv != s->ip) { closeRun(n); s->ip = sv; s->room.clear(); SaveGraph(); } return true; }
            if (key == "port") { if (!isInt || iv < 1024 || iv > 65535) return bad("Port 1024–65535"); if (iv != (int)s->port) { closeRun(n); s->port = iv; SaveGraph(); } return true; }
            if (key == "scheme") { if (!isInt || iv < 0 || iv > 1) return bad("Ungültig"); closeRun(n); s->scheme = iv; SaveGraph(); return true; }
            if (key == "rate") { if (!isInt || (iv != 44100 && iv != 48000)) return bad("Ungültig"); closeRun(n); s->rate = iv; SaveGraph(); return true; }
            if (key == "burst") { if (!isInt || iv < 200 || iv > 8000 || iv % 100) return bad("Ungültig"); s->burstMs = iv; WebSaveLater(); return true; }
            if (key == "fmt") { if (!isInt || iv < 0 || iv > 1) return bad("Ungültig"); closeRun(n); s->format = iv == 0 ? 1 : 0; if (g_sel == n) UpdateInspector(); SaveGraph(); return true; }
            if (key == "volCtl") { if (!hasB) return bad("Ungültig"); s->volCtl = b; if (g_sel == n) UpdateInspector(); SaveGraph(); return true; }
            if (key == "dither") { if (!hasB) return bad("Ungültig"); s->dither = b; SaveGraph(); return true; }
            if (key == "autoLat") { if (!hasB) return bad("Ungültig"); s->autoLat = b; s->measState = 0; SaveGraph(); return true; }
            if (key == "vol") { if (!isInt || iv < 0 || iv > 100 || !(bool)s->volCtl) return bad("Lautstärke-Steuerung ist aus"); s->sonosVol = iv; WebSaveLater(); return true; }
            if (key == "play") { if (!hasB) return bad("Ungültig"); if (b) { closeRun(n); s->playWanted = true; g_msg = L"Stream wird neu gestartet."; } else { s->playWanted = false; g_msg = L"Sonos-Wiedergabe gestoppt (Server läuft weiter)."; } return true; } }
        if (n->kind == K_AIRPLAY && n->ap) { AirplayCtx* a = n->ap;
            if (key == "name") { std::wstring nm = CleanText(W8(sv)); if (!hasS || nm.empty() || nm.size() > 40) return bad("Name 1–40 Zeichen"); if (nm != a->name) { closeRun(n); a->name = nm; n->name = L"AirPlay: " + nm; UpdateInspector(); SaveGraph(); } return true; }
            if (key == "port") { if (!isInt || iv < 1024 || iv > 65535) return bad("Port 1024–65535"); if (iv != (int)a->port) { closeRun(n); a->port = iv; SaveGraph(); } return true; }
            if (key == "buf") { if (!isInt || iv < 50 || iv > 2000) return bad("50–2000 ms"); a->bufferMs = iv; WebSaveLater(); return true; }
            if (key == "latAuto") { if (!hasB) return bad("Ungültig"); a->latAuto = b; SaveGraph(); return true; }
            if (key == "latMs") { if (!isInt || iv < 0 || iv > 4000) return bad("0–4000 ms"); a->latManualMs = iv; WebSaveLater(); return true; }
            if (key == "useVol") { if (!hasB) return bad("Ungültig"); a->useVol = b; SaveGraph(); return true; } }
        return bad("Einstellung unbekannt"); }
    return bad("Unbekannter Befehl");
}
struct AppBackend : web::Backend {
    std::mutex cm; std::string cache;
    std::string StateFields() override {
        WebReq r; r.kind = 1; DWORD_PTR res = 0;
        if (!g_webQuit && SendMessageTimeoutW(g_hwnd, WM_WEBCMD, 0, (LPARAM)&r, SMTO_ABORTIFHUNG, 1500, &res) && r.ok) { std::lock_guard<std::mutex> l(cm); cache = r.out; }
        std::lock_guard<std::mutex> l(cm); return cache.empty() ? "\"running\":false,\"autopilot\":false,\"nodes\":[],\"conns\":[],\"scenes\":[]" : cache;
    }
    std::string MeterJson() override {
        std::string o = "{\"t\":\"m\",\"a\":["; bool f = true; EnterCriticalSection(&g_cs);
        for (Node* n : g_nodes) { char b[96]; snprintf(b, sizeof b, "%s[%d,%.1f,%.1f,%d]", f ? "" : ",", n->id, n->dispL, n->dispR, n->clip ? 1 : 0); o += b; f = false; }
        LeaveCriticalSection(&g_cs); return o + "]}";
    }
    void OnBinary(const std::string&, const std::string& d) override { if (!g_webQuit) WebMicChunk(d); }
    bool Exec(const web::JObj& cmd, std::string& err) override {
        if (g_webQuit) { err = "Programm wird beendet"; return false; } WebReq r; r.cmd = cmd; DWORD_PTR res = 0;
        if (!SendMessageTimeoutW(g_hwnd, WM_WEBCMD, 0, (LPARAM)&r, SMTO_ABORTIFHUNG, 4000, &res)) { err = "Router reagiert nicht"; return false; }
        if (!r.ok) err = r.err; return r.ok;
    }
};
static AppBackend g_webBe; static web::Server* g_web = nullptr; static webwin::Host g_webHost; static std::string g_webErr, g_webCode; static int64_t g_webCodeExp = 0;
static std::string WebUrl() { auto ips = webwin::LocalIPv4(); return "https://" + (ips.empty() ? std::string("127.0.0.1") : ips[0]) + ":" + std::to_string(g_webPort) + "/"; }
static void WebNewCode() { if (g_web) { g_webCode = g_web->NewPairCode(); g_webCodeExp = web::NowSec() + g_web->pairTtlSec; } }
static bool StartWeb() {
    if (g_web) return true; g_webErr.clear(); webwin::LogHook() = [](const std::string& s) { Log("%s", s.c_str()); }; webwin::InitRandom(); g_webQuit = false;
    g_web = new web::Server(&g_webBe, U8(AppDir() + L"\\web")); g_web->version = U8(APP_VERSION); CreateDirectoryW((AppDir() + L"\\web").c_str(), nullptr);
    g_web->SetIndex(std::string((const char*)WEB_INDEX, WEB_INDEX_LEN)); g_web->AddAsset("/sw.js", "text/javascript", std::string((const char*)WEB_SW, WEB_SW_LEN)); g_web->AddAsset("/manifest.webmanifest", "application/manifest+json", std::string((const char*)WEB_MANIFEST, WEB_MANIFEST_LEN));
    g_web->AddAsset("/icon-192.png", "image/png", std::string((const char*)WEB_ICON192, WEB_ICON192_LEN), true);
    for (const char* p : {"/apple-touch-icon.png", "/apple-touch-icon-precomposed.png", "/icon-180.png"}) g_web->AddAsset(p, "image/png", std::string((const char*)WEB_ICON180, WEB_ICON180_LEN), true);
    g_web->AddAsset("/icon-32.png", "image/png", std::string((const char*)WEB_ICON32, WEB_ICON32_LEN), true); g_web->AddAsset("/favicon.ico", "image/x-icon", std::string((const char*)WEB_FAVICON, WEB_FAVICON_LEN), true); g_web->AddAsset("/icon-512.png", "image/png", std::string((const char*)WEB_ICON512, WEB_ICON512_LEN), true);
    std::string e; if (!g_webHost.Start(g_webPort, g_web, AppDir() + L"\\web", e)) { g_webErr = e; Log("Web-Interface: Start fehlgeschlagen – %s", e.c_str()); delete g_web; g_web = nullptr; return false; }
    WebNewCode(); TrayUpdate(); return true;
}
static void StopWeb() { g_webQuit = true; g_webHost.Stop(); delete g_web; g_web = nullptr; g_webCode.clear(); TrayUpdate(); }
static void WebAutostart() { g_webPort = std::max(1024, std::min(65535, _wtoi(IniGet(L"Web", L"Port", L"8443").c_str()))); if (IniGet(L"Web", L"Enabled", L"0") == L"1") StartWeb(); }

// ---- Einstellungsdialog: ein/aus, Port, QR-Kopplung, Geraeteliste, CA-Download
enum { IDW_TG = 6001, IDW_PORT, IDW_NEW, IDW_CA, IDW_REVOKE, IDW_LIST, IDW_STATUS, IDW_CODE, IDW_URL, IDW_HELP, IDW_LEFT };
static HWND g_webDlg = nullptr; static std::string g_webDevCache; static HFONT g_webBig = nullptr;
static const int WQX = 20, WQY = 168, WQS = 250;
static HWND WMk(HWND p, const wchar_t* cls, const wchar_t* txt, DWORD st, int x, int y, int w, int h, int id) { HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | st, S(x), S(y), S(w), S(h), p, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr); SendMessageW(c, WM_SETFONT, (WPARAM)g_fUI, TRUE); return c; }
static void WebDlgRefresh(HWND h) {
    std::wstring st = g_web ? L"läuft · " + W8(WebUrl()) : (!g_webErr.empty() ? L"Fehler: " + W8(g_webErr) : L"aus"); SetWindowTextW(GetDlgItem(h, IDW_STATUS), st.c_str());
    int64_t left = g_webCodeExp - web::NowSec(); wchar_t b[96];
    if (g_web && !g_webCode.empty() && left > 0) { std::wstring c = W8(g_webCode.substr(0, 4)) + L" " + W8(g_webCode.substr(4)); SetWindowTextW(GetDlgItem(h, IDW_CODE), c.c_str());
        swprintf(b, 96, L"gültig noch %d:%02d Min, einmal verwendbar", (int)(left / 60), (int)(left % 60)); SetWindowTextW(GetDlgItem(h, IDW_LEFT), b); }
    else { SetWindowTextW(GetDlgItem(h, IDW_CODE), L"–"); SetWindowTextW(GetDlgItem(h, IDW_LEFT), g_web ? L"Code abgelaufen – neuen erzeugen" : L"Web-Interface ist aus"); }
    SetWindowTextW(GetDlgItem(h, IDW_URL), g_web ? W8(WebUrl()).c_str() : L"");
    std::string dc; if (g_web) for (auto& d : g_web->Devices()) dc += d.id + "|" + d.name + "\n";
    if (dc != g_webDevCache) { g_webDevCache = dc; HWND lb = GetDlgItem(h, IDW_LIST); SendMessageW(lb, LB_RESETCONTENT, 0, 0); if (g_web) for (auto& d : g_web->Devices()) { int i = (int)SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)W8(d.name).c_str()); SendMessageW(lb, LB_SETITEMDATA, i, (LPARAM)new std::string(d.id)); } }
    RECT q = {S(WQX), S(WQY), S(WQX + WQS), S(WQY + WQS)}; InvalidateRect(h, &q, FALSE);
}
static LRESULT CALLBACK WebDlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        if (!g_webBig) { LOGFONTW lf = {}; lf.lfHeight = -S(34); lf.lfWeight = FW_BOLD; wcscpy(lf.lfFaceName, L"Consolas"); g_webBig = CreateFontIndirectW(&lf); }
        HWND t = WMk(h, L"STATIC", L"Web-Interface – Router per Handy bedienen", SS_LEFT, 20, 14, 520, 22, 0); SendMessageW(t, WM_SETFONT, (WPARAM)g_fBold, TRUE);
        HWND tg = CreateWindowExW(0, L"BARToggle", L"Web-Interface einschalten (nur HTTPS, nur im privaten Netz)", WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(20), S(46), S(520), S(28), h, (HMENU)(INT_PTR)IDW_TG, GetModuleHandleW(nullptr), nullptr); SendMessageW(tg, WM_SETFONT, (WPARAM)g_fUI, TRUE); SendMessageW(tg, TGM_SET, g_web ? 1 : 0, 0);
        WMk(h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 20, 82, 520, 20, IDW_STATUS); WMk(h, L"STATIC", L"Port:", SS_LEFT, 20, 118, 40, 20, 0);
        HWND pe = WMk(h, L"EDIT", std::to_wstring(g_webPort).c_str(), WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, 64, 114, 80, 26, IDW_PORT); (void)pe; WMk(h, L"STATIC", L"Firewall-Abfrage von Windows bitte für „Private Netzwerke“ erlauben.", SS_LEFT, 156, 118, 400, 20, 0);
        WMk(h, L"STATIC", L"Kopplungscode", SS_LEFT, 300, WQY, 240, 18, 0); HWND cd = WMk(h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 300, WQY + 22, 250, 48, IDW_CODE); SendMessageW(cd, WM_SETFONT, (WPARAM)g_webBig, TRUE);
        WMk(h, L"STATIC", L"", SS_LEFT, 300, WQY + 74, 250, 36, IDW_LEFT); HWND b1 = CreateWindowExW(0, L"BARBtn", L"Neuen Code erzeugen", WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(300), S(WQY + 120), S(240), S(36), h, (HMENU)(INT_PTR)IDW_NEW, GetModuleHandleW(nullptr), nullptr); SendMessageW(b1, WM_SETFONT, (WPARAM)g_fUI, TRUE); SendMessageW(b1, BTM_SETFLAGS, BTN_ACCENT, 0);
        HWND b2 = CreateWindowExW(0, L"BARBtn", L"CA-Zertifikat speichern…", WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(300), S(WQY + 166), S(240), S(36), h, (HMENU)(INT_PTR)IDW_CA, GetModuleHandleW(nullptr), nullptr); SendMessageW(b2, WM_SETFONT, (WPARAM)g_fUI, TRUE);
        WMk(h, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 20, WQY + WQS + 10, 520, 20, IDW_URL);
        WMk(h, L"STATIC", L"So koppelst du ein Handy (gleiches WLAN):\r\n1. QR-Code mit der Kamera scannen – die Web-App öffnet sich.\r\n2. Einmalig die Zertifikatswarnung bestätigen und \"/ca.crt\" öffnen, das CA-Zertifikat installieren (Android: Einstellungen → Zertifikat installieren; iPhone: Profil laden, dann Info → Zertifikatsvertrauen aktivieren). Danach keine Warnung mehr und „Zum Home-Bildschirm“ möglich.\r\n3. Code kommt per QR automatisch; sonst oben eingeben.", SS_LEFT, 20, WQY + WQS + 36, 520, 128, IDW_HELP);
        WMk(h, L"STATIC", L"Gekoppelte Geräte:", SS_LEFT, 20, WQY + WQS + 172, 300, 18, 0); WMk(h, L"LISTBOX", L"", WS_BORDER | LBS_NOTIFY | WS_VSCROLL, 20, WQY + WQS + 194, 380, 100, IDW_LIST);
        HWND b3 = CreateWindowExW(0, L"BARBtn", L"Gerät abmelden", WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(412), S(WQY + WQS + 194), S(128), S(36), h, (HMENU)(INT_PTR)IDW_REVOKE, GetModuleHandleW(nullptr), nullptr); SendMessageW(b3, WM_SETFONT, (WPARAM)g_fUI, TRUE);
        SetTimer(h, 1, 1000, nullptr); WebDlgRefresh(h); return 0; }
    case WM_TIMER: WebDlgRefresh(h); return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDW_TG) { bool on = SendMessageW(GetDlgItem(h, IDW_TG), TGM_GET, 0, 0) != 0;
            if (on) { wchar_t pb[16] = {}; GetWindowTextW(GetDlgItem(h, IDW_PORT), pb, 16); g_webPort = std::max(1024, std::min(65535, _wtoi(pb))); IniSet(L"Web", L"Port", std::to_wstring(g_webPort)); if (!StartWeb()) { SendMessageW(GetDlgItem(h, IDW_TG), TGM_SET, 0, 0); on = false; } }
            else StopWeb();
            IniSet(L"Web", L"Enabled", on ? L"1" : L"0"); g_webDevCache = "?"; WebDlgRefresh(h); }
        else if (id == IDW_NEW && g_web) { WebNewCode(); WebDlgRefresh(h); }
        else if (id == IDW_CA && g_web) { OPENFILENAMEW ofn = {}; wchar_t path[MAX_PATH] = L"bens-audio-router-ca.crt"; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = h; ofn.lpstrFilter = L"Zertifikat (*.crt)\0*.crt\0"; ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH; ofn.lpstrDefExt = L"crt"; ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&ofn)) { HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); if (f != INVALID_HANDLE_VALUE) { std::string pem = g_webHost.certs().caPem; DWORD k; WriteFile(f, pem.data(), (DWORD)pem.size(), &k, nullptr); CloseHandle(f); g_msg = L"CA-Zertifikat gespeichert."; } } }
        else if (id == IDW_REVOKE && g_web) { HWND lb = GetDlgItem(h, IDW_LIST); int i = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0); if (i >= 0) { auto* d = (std::string*)SendMessageW(lb, LB_GETITEMDATA, i, 0); if (d) g_web->Revoke(*d); g_webDevCache = "?"; WebDlgRefresh(h); } }
        return 0; }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT r; GetClientRect(h, &r); HBRUSH bg = CreateSolidBrush(TH.bg); FillRect(dc, &r, bg); DeleteObject(bg);
        RECT qr_ = {S(WQX), S(WQY), S(WQX + WQS), S(WQY + WQS)}; HBRUSH wh = CreateSolidBrush(RGB(255, 255, 255)); FillRect(dc, &qr_, wh); DeleteObject(wh);
        if (g_web && !g_webCode.empty() && g_webCodeExp > web::NowSec()) {
            qr::Code code; if (qr::Encode(WebUrl() + "#code=" + g_webCode, code)) { int mod = std::max(1, S(WQS) / (code.size + 6)), off = (S(WQS) - mod * code.size) / 2; HBRUSH bk = CreateSolidBrush(RGB(0, 0, 0));
                for (int y = 0; y < code.size; y++) for (int x = 0; x < code.size; x++) if (code.get(x, y)) { RECT m_ = {S(WQX) + off + x * mod, S(WQY) + off + y * mod, S(WQX) + off + (x + 1) * mod, S(WQY) + off + (y + 1) * mod}; FillRect(dc, &m_, bk); } DeleteObject(bk); } }
        EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLORSTATIC: SetTextColor((HDC)w, TH.text); SetBkColor((HDC)w, TH.bg); return (LRESULT)g_brBg;
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: SetTextColor((HDC)w, TH.text); SetBkColor((HDC)w, TH.surface); return (LRESULT)g_brSurface;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: { HWND lb = GetDlgItem(h, IDW_LIST); for (int i = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0) - 1; i >= 0; i--) delete (std::string*)SendMessageW(lb, LB_GETITEMDATA, i, 0); KillTimer(h, 1); g_webDlg = nullptr; return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}
static void WebDlgOpen() {
    if (g_webDlg) { SetForegroundWindow(g_webDlg); return; } static bool reg = false;
    if (!reg) { WNDCLASSEXW c = {sizeof(c)}; c.lpfnWndProc = WebDlgProc; c.hInstance = GetModuleHandleW(nullptr); c.hCursor = LoadCursorW(nullptr, IDC_ARROW); c.lpszClassName = L"BARWebDlg"; c.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1)); RegisterClassExW(&c); reg = true; }
    RECT pr; GetWindowRect(g_hwnd, &pr); g_webDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"BARWebDlg", L"Web-Interface", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, pr.left + S(80), pr.top + S(60), S(580), S(780), g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    BOOL d = TH.dark; DwmSetWindowAttribute(g_webDlg, 20, &d, sizeof d);
}

// ======================================================================= Hintergrund-Betrieb: Tray-Symbol, Autostart, Schliessen = im Tray weiterlaufen
static const UINT WM_TRAY = WM_APP + 41, WM_SHOWME = WM_APP + 42;
static UINT g_wmTaskbar = 0; static bool g_quit = false, g_trayOn = false, g_trayHinted = false;
enum { TM_OPEN = 9101, TM_ENGINE, TM_WEB, TM_AUTOSTART, TM_CLOSETRAY, TM_HIDDEN, TM_AUTOENGINE, TM_QUIT };
static bool BgFlag(const wchar_t* k, bool def) { return IniGet(L"App", k, def ? L"1" : L"0") == L"1"; }
static void BgSet(const wchar_t* k, bool v) { IniSet(L"App", k, v ? L"1" : L"0"); }
static const wchar_t* RUNKEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static bool AutostartOn() { wchar_t b[2 * MAX_PATH]; DWORD n = sizeof b; return RegGetValueW(HKEY_CURRENT_USER, RUNKEY, L"BensAudioRouter", RRF_RT_REG_SZ, nullptr, b, &n) == ERROR_SUCCESS; }
static void AutostartSet(bool on) {
    HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) { wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH); std::wstring v = L"\"" + std::wstring(p) + L"\" --background"; RegSetValueExW(k, L"BensAudioRouter", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))); }
    else RegDeleteValueW(k, L"BensAudioRouter");
    RegCloseKey(k);
}
static NOTIFYICONDATAW TrayData() { NOTIFYICONDATAW d = {}; d.cbSize = sizeof d; d.hWnd = g_hwnd; d.uID = 1; d.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; d.uCallbackMessage = WM_TRAY; d.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1)); return d; }
static void TrayUpdate() {
    if (!g_hwnd) return; NOTIFYICONDATAW d = TrayData(); std::wstring tip = std::wstring(APP_NAME) + L" – " + (g_running ? L"Engine läuft" : L"Engine gestoppt") + (g_web ? L" · Web-Interface an" : L"");
    wcsncpy(d.szTip, tip.c_str(), 127); if (!g_trayOn) g_trayOn = Shell_NotifyIconW(NIM_ADD, &d) != FALSE; else Shell_NotifyIconW(NIM_MODIFY, &d);
}
static void TrayBalloon(const wchar_t* title, const wchar_t* text) { NOTIFYICONDATAW d = TrayData(); d.uFlags = NIF_INFO; wcsncpy(d.szInfoTitle, title, 63); wcsncpy(d.szInfo, text, 255); d.dwInfoFlags = NIIF_INFO; Shell_NotifyIconW(NIM_MODIFY, &d); }
static void ShowMain() { ShowWindow(g_hwnd, SW_SHOW); if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE); SetForegroundWindow(g_hwnd); }
static void BgCmd(int cmd) {
    switch (cmd) {
    case TM_OPEN: ShowMain(); break;
    case TM_ENGINE: HandleCommand(IDC_BTN_START, BN_CLICKED, nullptr); break;
    case TM_WEB: WebDlgOpen(); break;
    case TM_AUTOSTART: { bool on = !AutostartOn(); AutostartSet(on); if (on) { BgSet(L"AutoEngine", true); BgSet(L"CloseToTray", true); g_msg = L"Startet mit Windows im Hintergrund, Engine startet automatisch, Schließen = Tray."; } else g_msg = L"Autostart aus."; break; }
    case TM_CLOSETRAY: BgSet(L"CloseToTray", !BgFlag(L"CloseToTray", false)); break;
    case TM_HIDDEN: BgSet(L"StartHidden", !BgFlag(L"StartHidden", true)); break;
    case TM_AUTOENGINE: BgSet(L"AutoEngine", !BgFlag(L"AutoEngine", false)); break;
    case TM_QUIT: g_quit = true; PostMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
    }
}
static void BgMenu(POINT p) {
    HMENU m = CreatePopupMenu(); auto chk = [](bool b) { return (UINT)(MF_STRING | (b ? MF_CHECKED : 0)); };
    AppendMenuW(m, MF_STRING, TM_OPEN, L"Fenster öffnen"); AppendMenuW(m, MF_STRING, TM_ENGINE, g_running ? L"Engine stoppen" : L"Engine starten"); AppendMenuW(m, MF_STRING, TM_WEB, L"Web-Interface…"); AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, chk(AutostartOn()), TM_AUTOSTART, L"Mit Windows starten (im Hintergrund)"); AppendMenuW(m, chk(BgFlag(L"CloseToTray", false)), TM_CLOSETRAY, L"Beim Schließen im Tray weiterlaufen");
    AppendMenuW(m, chk(BgFlag(L"StartHidden", true)), TM_HIDDEN, L"Beim Autostart nur Tray-Symbol (Fenster versteckt)"); AppendMenuW(m, chk(BgFlag(L"AutoEngine", false)), TM_AUTOENGINE, L"Engine beim Programmstart automatisch starten");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr); AppendMenuW(m, MF_STRING, TM_QUIT, L"Beenden");
    SetForegroundWindow(g_hwnd); int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_hwnd, nullptr); DestroyMenu(m); PostMessageW(g_hwnd, WM_NULL, 0, 0); if (cmd) BgCmd(cmd);
}
static void ShowBgMenu() { RECT r; GetWindowRect(g_btnBg, &r); BgMenu(POINT{r.left, r.bottom + 2}); }
static BOOL CALLBACK FindMainProc(HWND h, LPARAM l) { wchar_t t[160] = {}; GetWindowTextW(h, t, 160); if (!wcsncmp(t, APP_NAME, wcslen(APP_NAME)) && (GetWindowLongPtrW(h, GWL_STYLE) & WS_THICKFRAME)) { *(HWND*)l = h; return FALSE; } return TRUE; }

static void AddToolNodeAt(int kind, const POINT* wp) {   // wp = Weltkoordinate (Drop) oder nullptr = automatisch platzieren
    Node* n = kind == K_SUM ? AddNode(K_SUM, L"sum", L"Summe (Mono → Stereo)", 0, 0) : AddNode(K_REC, L"rec", L"Aufnahme-Box", 0, 0);
    if (wp) {
        n->x = (float)(std::floor((wp->x - NODE_W / 2) / 10.0) * 10);
        n->y = (float)(std::floor((wp->y - 12) / 10.0) * 10);
    } else PlaceNodeAuto(n, kind == K_REC);
    SelectNode(n, nullptr);
    SaveGraph();
    g_msg = kind == K_SUM ? L"Summen-Modul hinzugefügt: Mono-Quellen in den linken Port ziehen, rechter Port → Ausgänge."
                          : L"Aufnahme-Box hinzugefügt: Quellen verbinden, Start drücken, dann „● REC“.";
}
static void BeginToolDrag(int kind) {
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) { AddToolNodeAt(kind, nullptr); return; }   // Maus schon losgelassen
    g_dragKind = kind;
    g_recDrag = true;
    g_ghostOn = false;
    SetCapture(g_hwnd);
}
static void EndRecDrag(bool drop) {
    POINT p;
    GetCursorPos(&p);
    ScreenToClient(g_canvas, &p);
    RECT rc;
    GetClientRect(g_canvas, &rc);
    bool inside = PtInRect(&rc, p) != 0;
    g_recDrag = false;
    g_ghostOn = false;
    if (GetCapture() == g_hwnd) ReleaseCapture();
    if (drop && inside) { POINT w = ToWorldPx(p.x, p.y); AddToolNodeAt(g_dragKind, &w); }
    InvalidateRect(g_canvas, nullptr, FALSE);
}

static void HandleCommand(int id, int code, HWND src) {
    if (g_inspBusy) return;
    Node* n = g_sel;
    bool isStream = n && n->kind == K_STREAM && n->st;
    switch (id) {
    case IDC_BTN_SRC: ShowAddMenu(false); break;
    case IDC_BTN_DST: ShowAddMenu(true); break;
    case IDC_BTN_STREAM: ShowStreamMenu(); break;
    case IDC_BTN_MORE: ShowMoreMenu(); break;
    case IDC_BTN_BG: ShowBgMenu(); break;
    case IDC_BTN_WEB: WebDlgOpen(); break;
    case IDC_BTN_REC:
    case IDC_BTN_SUM:
        if (code == BN_DRAGBEGIN) BeginToolDrag(id == IDC_BTN_SUM ? K_SUM : K_REC);
        else AddToolNodeAt(id == IDC_BTN_SUM ? K_SUM : K_REC, nullptr);   // Klick: automatisch platzieren (Ziehen aufs Feld geht auch)
        break;
    case IDC_BTN_VIRT: ShowVirtualMenu(); break;
    case IDC_BTN_ABOUT: ShowAbout(); break;
    case IDC_BTN_START:
        if (g_running) StopEngine(); else StartEngine();
        SetStartButton();
        break;
    case IDC_BTN_LAYOUT: ShowLayoutMenu(); break;
    case IDC_BTN_THEME: ShowThemeMenu(); break;
    case IDC_BTN_RESCAN: EnumerateDevices(); StartDiscovery(); g_msg = L"Geräteliste aktualisiert, Sonos-Suche gestartet."; break;
    case IDC_BTN_SYNCNOW: StartCalibration(); break;
    case IDC_TG_SYNC:
        g_autoSync = SendMessageW(g_tgSync, TGM_GET, 0, 0) != 0;
        if (g_autoSync) RecomputeSync();
        UpdateInspector();
        SaveGraph();
        break;
    case IDC_CMB_PAD: if (code == CBN_SELCHANGE) { g_padMs = PAD_VALS[std::max(0, (int)SendMessageW(g_cmbPad, CB_GETCURSEL, 0, 0))]; SaveGraph(); } break;
    case IDC_CMB_BASE: if (code == CBN_SELCHANGE) { g_baseMs = BASE_VALS[std::max(0, (int)SendMessageW(g_cmbBase, CB_GETCURSEL, 0, 0))]; SaveGraph(); } break;
    case IDC_CMB_POLL: if (code == CBN_SELCHANGE) { g_pollMs = POLL_VALS[std::max(0, (int)SendMessageW(g_cmbPoll, CB_GETCURSEL, 0, 0))]; SaveGraph(); } break;
    case IDC_CMB_DRV:
        if (code == CBN_SELCHANGE) {
            g_drvMs = DRV_VALS[std::max(0, (int)SendMessageW(g_cmbDrv, CB_GETCURSEL, 0, 0))];
            SaveGraph();
            g_msg = L"Treiber-Puffer greift beim nächsten Start bzw. „Neu verbinden“.";
        }
        break;
    case IDC_I_GAIN:
        if (n && (code == SLN_CHANGING || code == SLN_DONE)) {
            int v = (int)SendMessageW(iGain, SLM_GETPOS, 0, 0);
            n->gainDb = v / 2.0f;
            n->gainLin = std::pow(10.0f, n->gainDb / 20.0f);
            SetWindowTextW(iGainVal, (FmtD(n->gainDb, 1) + L" dB").c_str());
            if (code == SLN_DONE) SaveGraph();
        }
        break;
    case IDC_I_MUTE:
        if (n) { n->mute = SendMessageW(iMute, TGM_GET, 0, 0) != 0; SaveGraph(); }
        break;
    case IDC_I_DLY_EDIT:
        if (n && code == EN_CHANGE) {
            ApplyDelay(n, ParseD(iDlyEdit));
            g_inspBusy = true;
            SendMessageW(iDlySl, SLM_SETPOS, (WPARAM)std::lround(std::min(2000.0, n->delayMs.load())), 0);
            g_inspBusy = false;
        } else if (code == EN_KILLFOCUS) SaveGraph();
        break;
    case IDC_I_DLY_SL:
        if (n && (code == SLN_CHANGING || code == SLN_DONE)) {
            ApplyDelay(n, (double)SendMessageW(iDlySl, SLM_GETPOS, 0, 0));
            g_inspBusy = true;
            SetWindowTextW(iDlyEdit, FmtD(n->delayMs.load(), 1).c_str());
            g_inspBusy = false;
            if (code == SLN_DONE) SaveGraph();
        }
        break;
    case IDC_I_TRIM_EDIT:
        if (n && code == EN_CHANGE) {
            ApplyTrim(n, ParseD(iTrimEdit));
            g_inspBusy = true;
            SendMessageW(iTrimSl, SLM_SETPOS, (WPARAM)std::lround(n->trimMs.load() * 10.0), 0);
            g_inspBusy = false;
        } else if (code == EN_KILLFOCUS) SaveGraph();
        break;
    case IDC_I_TRIM_SL:
        if (n && (code == SLN_CHANGING || code == SLN_DONE)) {
            ApplyTrim(n, (double)SendMessageW(iTrimSl, SLM_GETPOS, 0, 0) / 10.0);
            g_inspBusy = true;
            SetWindowTextW(iTrimEdit, FmtD(n->trimMs.load(), 1).c_str());
            g_inspBusy = false;
            if (code == SLN_DONE) SaveGraph();
        }
        break;
    case IDC_I_EXT_EDIT:
        if (n && code == EN_CHANGE) n->extMs = std::max(0.0, ParseD(iExtEdit));
        else if (code == EN_KILLFOCUS) SaveGraph();
        break;
    case IDC_I_SUMMONO:
        if (n && n->kind == K_SUM) { n->sumMono = SendMessageW(iSumMono, TGM_GET, 0, 0) != 0; SaveGraph(); }
        break;
    case IDC_I_SUMSRC0 ... IDC_I_SUMSRC0 + 5:
    case IDC_I_SUMDST0 ... IDC_I_SUMDST0 + 5:
        if (n && n->kind == K_SUM && code == CBN_SELCHANGE) {
            int i = id >= IDC_I_SUMDST0 ? id - IDC_I_SUMDST0 : id - IDC_I_SUMSRC0;
            std::vector<Conn*> in = SumInputs(n);
            if (i < (int)in.size()) {
                in[i]->srcCh = std::max(0, std::min(3, (int)SendMessageW(iSumSrc[i], CB_GETCURSEL, 0, 0)));
                in[i]->dstCh = std::max(0, std::min(3, (int)SendMessageW(iSumDst[i], CB_GETCURSEL, 0, 0)));
                SaveGraph();
            }
        }
        break;
    case IDC_I_RECBTN:
        if (n && n->rc) {
            RecToggle(n);
            if (!n->rc->rec.active() && !n->rc->err.empty()) g_msg = n->rc->err;
            RefreshRecCtl(n);
        }
        break;
    case IDC_I_RECFMT:
        if (n && n->rc && code == CBN_SELCHANGE && !n->rc->rec.active()) {
            int i = (int)SendMessageW(iRecFmt, CB_GETCURSEL, 0, 0);
            n->rc->flac = (i == 1 || i == 2);   // 0 WAV24, 1 FLAC24, 2 FLAC16, 3 WAV16, 4 WAV32f
            n->rc->bits = (i == 2 || i == 3) ? 16 : i == 4 ? 32 : 24;
            RefreshRecCtl(n);
            SaveGraph();
        }
        break;
    case IDC_I_RECRATE:
        if (n && n->rc && code == CBN_SELCHANGE && !n->rc->rec.active()) {
            static const int R[4] = {44100, 48000, 88200, 96000};
            int i = std::max(0, std::min(3, (int)SendMessageW(iRecRate, CB_GETCURSEL, 0, 0)));
            n->rc->rate = R[i];
            n->rate = R[i];
            ReopenNode(n);   // Takt der Box mit neuer Samplerate neu aufsetzen
            RefreshRecCtl(n);
            SaveGraph();
        }
        break;
    case IDC_I_RECDIR:
        if (n && n->rc && code == EN_KILLFOCUS) {
            wchar_t b[1000] = {};
            GetWindowTextW(iRecDir, b, 1000);
            if (b[0] && n->rc->dir != b) { n->rc->dir = b; SaveGraph(); }
            RefreshRecCtl(n);
        }
        break;
    case IDC_I_RECBROWSE:
        if (n && n->rc && !n->rc->rec.active()) {
            wchar_t disp[MAX_PATH] = {};
            BROWSEINFOW bi = {};
            bi.hwndOwner = g_hwnd;
            bi.pszDisplayName = disp;
            bi.lpszTitle = L"Zielordner für Aufnahmen wählen";
            bi.ulFlags = BIF_RETURNONLYFSDIRS;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t path[MAX_PATH] = {};
                if (SHGetPathFromIDListW(pidl, path) && path[0]) { n->rc->dir = path; SetWindowTextW(iRecDir, path); SaveGraph(); RefreshRecCtl(n); }
                CoTaskMemFree(pidl);
            }
        }
        break;
    case IDC_I_RECOPEN:
        if (n && n->rc) {
            std::wstring d = n->rc->dir.empty() ? RecDefaultDir() : n->rc->dir;
            RecMkDirs(d);
            ShellExecuteW(nullptr, L"open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    case IDC_I_GEN:
        if (n && code == CBN_SELCHANGE && n->kind == K_GEN) {
            n->genType = std::max(0, (int)SendMessageW(iGen, CB_GETCURSEL, 0, 0));
            const wchar_t* ids[] = {L"gen:click", L"gen:sine", L"gen:pink"};
            const wchar_t* nm[] = {L"Test: Klick (1 Hz)", L"Test: Sinus 1 kHz", L"Test: Rosa Rauschen"};
            n->devId = ids[n->genType];
            n->name = nm[n->genType];
            UpdateInspector();
            SaveGraph();
        }
        break;
    case IDC_I_SONOS:
        if (isStream && code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(iSonos, CB_GETCURSEL, 0, 0);
            SonosDev d;
            bool ok = false;
            EnterCriticalSection(&g_sonosCs);
            if (i >= 0 && i < (int)g_sonos.size()) { d = g_sonos[i]; ok = true; }
            LeaveCriticalSection(&g_sonosCs);
            if (ok) {
                if (g_running) { EnterCriticalSection(&g_cs); CloseNode(n); LeaveCriticalSection(&g_cs); }
                n->st->ip = d.ip;
                n->st->room = d.name;
                n->name = L"Stream: " + d.name;
                UpdateInspector();
                SaveGraph();
            }
        }
        break;
    case IDC_I_GRPADD:
        if (isStream) {
            int i = (int)SendMessageW(iGrpCmb, CB_GETCURSEL, 0, 0);
            SonosDev d;
            bool ok = false;
            EnterCriticalSection(&g_sonosCs);
            if (i >= 0 && i < (int)g_sonos.size()) { d = g_sonos[i]; ok = true; }
            LeaveCriticalSection(&g_sonosCs);
            if (ok && d.ip != n->st->ip) {
                EnterCriticalSection(&n->st->cs);
                if (std::find(n->st->groupIps.begin(), n->st->groupIps.end(), d.ip) == n->st->groupIps.end()) {
                    n->st->groupIps.push_back(d.ip);
                    n->st->groupNames.push_back(d.name);
                }
                LeaveCriticalSection(&n->st->cs);
                n->st->groupDirty = true;
                g_msg = L"Zone zur Gruppe hinzugefügt – Sonos synchronisiert sie selbst.";
                UpdateGroupList();
                SaveGraph();
            } else if (ok) g_msg = L"Die Haupt-Zone dieses Nodes ist schon der Koordinator.";
            else g_msg = L"Bitte zuerst über „Suchen“ eine Zone finden und auswählen.";
        }
        break;
    case IDC_I_GRPCLR:
        if (isStream) {
            EnterCriticalSection(&n->st->cs);
            n->st->groupIps.clear();
            n->st->groupNames.clear();
            n->st->groupVol.clear();
            n->st->groupWant.clear();
            LeaveCriticalSection(&n->st->cs);
            n->st->groupDirty = true;
            UpdateGroupList();
            SaveGraph();
        }
        break;
    case IDC_I_APNAME:
        if (n && n->ap && code == EN_KILLFOCUS) {
            wchar_t b[128] = {};
            GetWindowTextW(iApName, b, 128);
            std::wstring nm = CleanText(b);
            if (!nm.empty() && nm != n->ap->name) {
                if (g_running) { EnterCriticalSection(&g_cs); CloseNode(n); LeaveCriticalSection(&g_cs); }
                n->ap->name = nm;
                n->name = L"AirPlay: " + nm;
                UpdateInspector();
                SaveGraph();
            }
        }
        break;
    case IDC_I_APPORT:
        if (n && n->ap && code == EN_KILLFOCUS) {
            wchar_t b[32] = {};
            GetWindowTextW(iApPort, b, 32);
            int p = std::max(1024, std::min(65535, _wtoi(b)));
            if (p != n->ap->port) {
                if (g_running) { EnterCriticalSection(&g_cs); CloseNode(n); LeaveCriticalSection(&g_cs); }
                n->ap->port = p;
                SaveGraph();
            }
        }
        break;
    case IDC_I_APBUF:
        if (n && n->ap && code == EN_CHANGE) {
            wchar_t b[32] = {};
            GetWindowTextW(iApBuf, b, 32);
            n->ap->bufferMs = std::max(50, std::min(2000, _wtoi(b)));
        } else if (code == EN_KILLFOCUS) SaveGraph();
        break;
    case IDC_I_APLATAUTO:
        if (n && n->ap) { n->ap->latAuto = SendMessageW(iApLatAuto, TGM_GET, 0, 0) != 0; EnableWindow(iApLatMs, !n->ap->latAuto.load()); SaveGraph(); }
        break;
    case IDC_I_APLATMS:
        if (n && n->ap && code == EN_CHANGE) {
            wchar_t b[32] = {};
            GetWindowTextW(iApLatMs, b, 32);
            n->ap->latManualMs = std::max(0, std::min(4000, _wtoi(b)));
        } else if (code == EN_KILLFOCUS) SaveGraph();
        break;
    case IDC_I_APVOL:
        if (n && n->ap) { n->ap->useVol = SendMessageW(iApVol, TGM_GET, 0, 0) != 0; SaveGraph(); }
        break;
    case IDC_I_SEARCH: StartDiscovery(); g_msg = L"Suche nach Sonos-Geräten …"; break;
    case IDC_I_IP:
        if (isStream && code == EN_KILLFOCUS) {
            std::string ip = GetTextU8(iIp);
            if (ip != n->st->ip) {
                if (g_running) { EnterCriticalSection(&g_cs); CloseNode(n); LeaveCriticalSection(&g_cs); }
                n->st->ip = ip;
                n->st->room.clear();
                SaveGraph();
            }
        }
        break;
    case IDC_I_PORT:
        if (isStream && code == EN_KILLFOCUS) {
            int p = std::max(1024, std::min(65535, _wtoi(std::wstring(GetTextU8(iPort).begin(), GetTextU8(iPort).end()).c_str())));
            if (p != n->st->port) {
                if (g_running) { EnterCriticalSection(&g_cs); CloseNode(n); LeaveCriticalSection(&g_cs); }
                n->st->port = p;
                SaveGraph();
            }
        }
        break;
    case IDC_I_SCHEME:
        if (isStream && code == CBN_SELCHANGE) {
            if (g_running) { EnterCriticalSection(&g_cs); CloseNode(n); LeaveCriticalSection(&g_cs); }
            n->st->scheme = std::max(0, (int)SendMessageW(iScheme, CB_GETCURSEL, 0, 0));
            SaveGraph();
        }
        break;
    case IDC_I_RATE:
        if (isStream && code == CBN_SELCHANGE) {
            if (g_running) { EnterCriticalSection(&g_cs); CloseNode(n); LeaveCriticalSection(&g_cs); }
            n->st->rate = SendMessageW(iRate, CB_GETCURSEL, 0, 0) == 1 ? 48000 : 44100;
            SaveGraph();
        }
        break;
    case IDC_I_BURST:
        if (isStream && (code == SLN_CHANGING || code == SLN_DONE)) {
            int v = (int)SendMessageW(iBurst, SLM_GETPOS, 0, 0) * 100;
            n->st->burstMs = v;
            SetWindowTextW(iBurstVal, (std::to_wstring(v) + L" ms").c_str());
            if (code == SLN_DONE) SaveGraph();
        }
        break;
    case IDC_I_FMT:
        if (isStream && code == CBN_SELCHANGE) {
            if (g_running) { EnterCriticalSection(&g_cs); CloseNode(n); LeaveCriticalSection(&g_cs); }
            n->st->format = SendMessageW(iFmt, CB_GETCURSEL, 0, 0) == 0 ? 1 : 0;
            UpdateInspector();
            SaveGraph();
        }
        break;
    case IDC_I_VOLCTL:
        if (isStream) { n->st->volCtl = SendMessageW(iVolCtl, TGM_GET, 0, 0) != 0; UpdateInspector(); SaveGraph(); }
        break;
    case IDC_I_DITHER:
        if (isStream) { n->st->dither = SendMessageW(iDither, TGM_GET, 0, 0) != 0; SaveGraph(); }
        break;
    case IDC_I_AUTOLAT:
        if (isStream) { n->st->autoLat = SendMessageW(iAutoLat, TGM_GET, 0, 0) != 0; n->st->measState = 0; SaveGraph(); }
        break;
    case IDC_I_VOL:
        if (isStream && n->st->volCtl.load() && (code == SLN_CHANGING || code == SLN_DONE)) {
            int v = (int)SendMessageW(iVol, SLM_GETPOS, 0, 0);
            n->st->sonosVol = v;
            SetWindowTextW(iVolVal, (std::to_wstring(v) + L" %").c_str());
            if (code == SLN_DONE) SaveGraph();
        }
        break;
    case IDC_I_PLAY:
        if (isStream) {
            if (g_running) { EnterCriticalSection(&g_cs); CloseNode(n); LeaveCriticalSection(&g_cs); }
            n->st->playWanted = true;
            g_msg = L"Stream wird neu gestartet.";
        }
        break;
    case IDC_I_STOPSON:
        if (isStream) { n->st->playWanted = false; g_msg = L"Sonos-Wiedergabe gestoppt (Server läuft weiter)."; }
        break;
    case IDC_I_RECONNECT:
        if (n) ReopenNode(n);
        break;
    case IDC_I_REMOVE: DeleteSelection(); break;
    default:
        if (id >= IDC_I_DLYB0 && id < IDC_I_DLYB0 + 6 && n) {
            ApplyDelay(n, n->delayMs.load() + DLY_VAL[id - IDC_I_DLYB0]);
            UpdateInspector();
            SaveGraph();
        } else if (id >= IDC_I_TRIMB0 && id < IDC_I_TRIMB0 + 4 && n) {
            ApplyTrim(n, n->trimMs.load() + TRIM_VAL[id - IDC_I_TRIMB0]);
            UpdateInspector();
            SaveGraph();
        }
    }
    (void)src;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_wmTaskbar && msg == g_wmTaskbar) { g_trayOn = false; TrayUpdate(); return 0; }   // Explorer neu gestartet: Tray-Symbol neu anlegen
    switch (msg) {
    case WM_CREATE: return 0;
    case WM_MOUSEMOVE:
        if (g_recDrag) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(g_canvas, &p);
            RECT rc;
            GetClientRect(g_canvas, &rc);
            g_ghostOn = PtInRect(&rc, p) != 0;
            g_ghostPx = p;
            InvalidateRect(g_canvas, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (g_recDrag) { SetCursor(LoadCursorW(nullptr, g_ghostOn ? IDC_CROSS : IDC_NO)); return TRUE; }
        break;
    case WM_CAPTURECHANGED:
        if (g_recDrag && (HWND)lp != hwnd) EndRecDrag(false);
        break;
    case WM_SIZE:
        if (g_canvas && wp != SIZE_MINIMIZED) LayoutMain(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_brBg);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int tb = S(TOOLBAR_H), sh = S(STATUS_H), iw = S(INSP_W);
        HPEN lp2 = CreatePen(PS_SOLID, 1, TH.border);
        HGDIOBJ op = SelectObject(dc, lp2);
        MoveToEx(dc, 0, tb - 1, nullptr);
        LineTo(dc, rc.right, tb - 1);
        MoveToEx(dc, rc.right - iw, tb, nullptr);
        LineTo(dc, rc.right - iw, rc.bottom - sh);
        MoveToEx(dc, 0, rc.bottom - sh, nullptr);
        LineTo(dc, rc.right, rc.bottom - sh);
        SelectObject(dc, op);
        DeleteObject(lp2);
        RECT sband = {0, rc.bottom - sh + 1, rc.right, rc.bottom};
        FillRect(dc, &sband, g_brBg);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, g_running ? TH.ok : TH.dim);
        RECT r0 = {S(14), rc.bottom - sh, S(130), rc.bottom};
        DrawTextW(dc, g_stText[0].c_str(), -1, &r0, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(dc, TH.dim);
        RECT r1 = {S(130), rc.bottom - sh, rc.right - S(700), rc.bottom};
        DrawTextW(dc, g_stText[1].c_str(), -1, &r1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT r2 = {rc.right - S(690), rc.bottom - sh, rc.right - S(300), rc.bottom};
        DrawTextW(dc, g_stText[2].c_str(), -1, &r2, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(dc, TH.accent);
        RECT r3 = {rc.right - S(290), rc.bottom - sh, rc.right - S(14), rc.bottom};
        DrawTextW(dc, APP_COPYRIGHT, -1, &r3, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND c = (HWND)lp;
        if (GetDlgCtrlID(c) == IDC_I_INFO) {
            SetTextColor(dc, TH.text);
            SetBkColor(dc, TH.surface);
            return (LRESULT)g_brSurface;
        }
        SetTextColor(dc, GetWindowLongPtrW(c, GWLP_USERDATA) == 1 ? TH.dim : TH.text);
        SetBkColor(dc, TH.bg);
        return (LRESULT)g_brBg;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, TH.text);
        SetBkColor(dc, TH.surface);
        return (LRESULT)g_brSurface;
    }
    case WM_SETTINGCHANGE:
        if (g_themeMode == 0 && lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) ApplyThemeNow();
        return 0;
    case WM_APP + 30:
        g_msg = L"FLAC wurde vom Sonos nicht akzeptiert – automatisch auf WAV 16 Bit umgestellt.";
        UpdateInspector();
        SaveGraph();
        return 0;
    case WM_CALIB_DONE:
        g_msg = g_calibMsg;
        if (g_autoSync) RecomputeSync();
        UpdateInspector();
        SaveGraph();
        return 0;
    case WM_SONOS_DONE:
        g_msg = L"Sonos-Suche beendet.";
        {   // gespeicherte Namen mit den frischen (bereinigten) Namen abgleichen
            std::vector<SonosDev> dev;
            EnterCriticalSection(&g_sonosCs);
            dev = g_sonos;
            LeaveCriticalSection(&g_sonosCs);
            for (Node* nn : g_nodes) {
                if (!nn->st) continue;
                for (auto& d : dev) {
                    if (d.ip == nn->st->ip && !d.name.empty()) {
                        if (nn->name.compare(0, 8, L"Stream: ") == 0) nn->name = L"Stream: " + d.name;
                        nn->st->room = d.name;
                    }
                    EnterCriticalSection(&nn->st->cs);
                    for (size_t i = 0; i < nn->st->groupIps.size() && i < nn->st->groupNames.size(); i++)
                        if (nn->st->groupIps[i] == d.ip && !d.name.empty()) nn->st->groupNames[i] = d.name;
                    LeaveCriticalSection(&nn->st->cs);
                }
            }
        }
        if (g_sel && g_sel->kind == K_STREAM) { g_inspBusy = true; FillSonosCombo(); FillGroupCombo(); g_inspBusy = false; }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT ir, cr;
        GetWindowRect(g_insp, &ir);
        GetWindowRect(g_canvas, &cr);
        if (PtInRect(&ir, pt)) {
            RECT rc;
            GetClientRect(g_insp, &rc);
            int maxScroll = std::max(0, g_contentH - (int)rc.bottom);
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            g_scrollY = std::max(0, std::min(maxScroll, g_scrollY - d * S(40) / 120));
            LayoutInspector();
            return 0;
        }
        if (PtInRect(&cr, pt)) return SendMessageW(g_canvas, WM_MOUSEWHEEL, wp, lp);
        return 0;
    }
    case WM_TIMER: {
        if (wp == 77) { KillTimer(hwnd, 77); SaveGraph(); return 0; }
        if (wp == 78) { KillTimer(hwnd, 78); if (!g_running) HandleCommand(IDC_BTN_START, BN_CLICKED, nullptr); return 0; }
        if (wp == 2) { KillTimer(hwnd, 2); StartCalibration(); return 0; }
        if (wp == 4) { KillTimer(hwnd, 4); SetupVirtual(); return 0; }
        if (wp == 3) { KillTimer(hwnd, 3); PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        if (g_calibRunning.load()) g_msg = g_calibMsg;
        {
            static bool lastCal = false;
            bool cr = g_calibRunning.load();
            if (cr != lastCal) { lastCal = cr; SetWindowTextW(g_btnSyncNow, cr ? L"Abbrechen" : L"Einmessen…"); }
        }
        static double last = NowSec();
        double now = NowSec();
        double dt = std::max(0.001, now - last);
        last = now;
        UpdateMeters(now, dt);
        if (g_autoSync) {
            RecomputeSync();
            if (g_sel && IsOutSink(g_sel->kind)) {
                wchar_t cur[64] = {};
                GetWindowTextW(iDlyEdit, cur, 64);
                if (FmtD(g_sel->delayMs.load(), 1) != cur) { g_inspBusy = true; SetWindowTextW(iDlyEdit, FmtD(g_sel->delayMs.load(), 1).c_str()); SendMessageW(iDlySl, SLM_SETPOS, (WPARAM)std::lround(std::min(2000.0, g_sel->delayMs.load())), 0); g_inspBusy = false; }
            }
        }
        InvalidateRect(g_canvas, nullptr, FALSE);
        static int tick = 0;
        if (++tick % 6 == 0) {
            RefreshInspectorInfo();
            UpdateStatusBar();
            SweepGraveyard();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_recDrag) { EndRecDrag(true); return 0; }
        RECT rc;
        GetClientRect(hwnd, &rc);
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (pt.y > rc.bottom - S(STATUS_H) && pt.x > rc.right - S(290)) ShellExecuteW(nullptr, L"open", APP_URL, nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    }
    case WM_COMMAND:
        HandleCommand(LOWORD(wp), HIWORD(wp), (HWND)lp);
        return 0;
    case WM_CLOSE:
        SaveSettings();
        SaveGraph();
        if (!g_quit && BgFlag(L"CloseToTray", false)) {   // Schliessen = im Hintergrund weiterlaufen
            ShowWindow(hwnd, SW_HIDE);
            if (!g_trayHinted) { g_trayHinted = true; TrayBalloon(APP_NAME, L"Läuft im Hintergrund weiter. Rechtsklick auf das Symbol: Beenden."); }
            return 0;
        }
        g_webQuit = true;
        StopWeb();
        StopEngine();
        if (IniGet(L"Virtual", L"AutoRestore", L"1") == L"1") RestoreDefaultRender(false);
        { NOTIFYICONDATAW d = TrayData(); Shell_NotifyIconW(NIM_DELETE, &d); }
        DestroyWindow(hwnd);
        return 0;
    case WM_TRAY: if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) ShowMain(); else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) { POINT p; GetCursorPos(&p); BgMenu(p); } return 0;
    case WM_SHOWME: ShowMain(); return 0;
    case WM_WEBCMD: { WebReq* r = (WebReq*)lp; r->ok = WebExecUi(*r); return 1; }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int nShow) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"BensAudioRouter3_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = nullptr; EnumWindows(FindMainProc, (LPARAM)&other);   // laufende Instanz (auch im Tray) nach vorn holen
        if (other) PostMessageW(other, WM_SHOWME, 0, 0); else MessageBoxW(nullptr, L"Bens Audio Router läuft bereits.", L"Bens Audio Router", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = f.QuadPart;
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_logCs);
    InitializeCriticalSection(&g_sonosCs);
    g_logT0 = NowSec();
    g_log = _wfopen((AppDir() + L"\\log.txt").c_str(), L"wb");
    Log("Bens Audio Router 4.5 gestartet (Buccloud Software (DE))");
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    timeBeginPeriod(1);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    SetProcessDPIAware();

    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    CreateFonts();
    LoadSettings();
    SetThemeColors(g_themeMode == 1 ? true : g_themeMode == 2 ? false : SystemUsesDark());
    ApplyDarkMenus(TH.dark);
    RegisterModernControls(hInst);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BAR3Main";
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    RegisterClassExW(&wc);
    WNDCLASSEXW cc = {};
    cc.cbSize = sizeof(cc);
    cc.lpfnWndProc = CanvasProc;
    cc.hInstance = hInst;
    cc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cc.lpszClassName = L"BAR3Canvas";
    RegisterClassExW(&cc);

    EnumerateDevices();
    LoadGraphFrom(GraphPath(), false);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, APP_NAME L" " APP_VERSION L" – " APP_PUBLISHER, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                             S(1420), S(840), nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;
    g_canvas = CreateWindowExW(0, cc.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, g_hwnd, nullptr, hInst, nullptr);
    g_insp = CreateWindowExW(WS_EX_CONTROLPARENT, L"BARPanel", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, g_hwnd, nullptr, hInst, nullptr);
    BuildToolbar();
    BuildInspector();
    SetStartButton();
    ShowGroups(false, 0);
    ApplyThemeNow();
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    LayoutMain(rc.right, rc.bottom);
    UpdateStatusBar();
    SetTimer(g_hwnd, 1, 33, nullptr);
    const bool hideStart = wcsstr(GetCommandLineW(), L"--background") != nullptr && BgFlag(L"StartHidden", true);   // Autostart: nur Tray-Symbol
    ShowWindow(g_hwnd, hideStart ? SW_HIDE : nShow);
    UpdateWindow(g_hwnd);
    g_wmTaskbar = RegisterWindowMessageW(L"TaskbarCreated"); TrayUpdate();
    StartDiscovery();
    WebAutostart();
    if (BgFlag(L"AutoEngine", false)) SetTimer(g_hwnd, 78, 2500, nullptr);
    {
        wchar_t ev[32] = {};
        if (GetEnvironmentVariableW(L"BAR_TEST_SIMDELAY", ev, 32)) g_simDelayMs = _wtof(ev);
        if (GetEnvironmentVariableW(L"BAR_TEST_AUTOYES", ev, 32)) g_autoYes = true;
    }
    if (cmdLine && wcsstr(cmdLine, L"/start")) { StartEngine(); SetStartButton(); }
    if (cmdLine && wcsstr(cmdLine, L"/calibrate")) SetTimer(g_hwnd, 2, 14000, nullptr);
    if (cmdLine && wcsstr(cmdLine, L"/setup")) SetTimer(g_hwnd, 4, 3000, nullptr);
    if (cmdLine && wcsstr(cmdLine, L"/quit")) SetTimer(g_hwnd, 3, 30000, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    timeEndPeriod(1);
    WSACleanup();
    CoUninitialize();
    if (g_log) fclose(g_log);
    if (mutex) CloseHandle(mutex);
    return 0;
}
