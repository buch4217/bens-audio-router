// vbus_win.h – Windows-Anbindung der virtuellen Bruecke (gemeinsam fuer Router und ASIO-Treiber)
// (MIT, © Ben Buchholz · Buccloud Software (DE))
// Beide Seiten legen den benannten Speicherblock bei Bedarf an (wer zuerst kommt); ein benannter Mutex schuetzt die Erstinitialisierung.
#pragma once
#include <windows.h>
#include "vbus.h"

namespace vbus_win {

inline uint64_t NowNs() {   // QPC in ns - systemweit gleiche Zeitbasis fuer Router und Treiber
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((long double)c.QuadPart * 1e9L / (long double)f.QuadPart);
}
// Verbindet mit dem Block (legt ihn an, falls er fehlt). Rueckgabe true bei Erfolg.
inline bool Attach(vbus::Header** hdr, HANDLE* map) {
    const size_t sz = vbus::TotalBytes(vbus::DEFAULT_RING);
    HANDLE mu = CreateMutexW(nullptr, FALSE, L"Local\\BensVBus1Lock");
    if (mu) WaitForSingleObject(mu, 2000);
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)((uint64_t)sz >> 32), (DWORD)(sz & 0xFFFFFFFFu), L"Local\\BensVBus1");
    bool ok = false;
    if (h) {
        void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sz);
        if (p) {
            vbus::Header* H = reinterpret_cast<vbus::Header*>(p);
            if (!vbus::Valid(H)) { std::memset(p, 0, sz); vbus::InitHeader(H); }   // erster Teilnehmer initialisiert (Speicher ist vom System genullt)
            *hdr = H; *map = h; ok = true;
        } else CloseHandle(h);
    }
    if (mu) { ReleaseMutex(mu); CloseHandle(mu); }
    return ok;
}
inline void Detach(vbus::Header** hdr, HANDLE* map) {
    if (*hdr) UnmapViewOfFile(*hdr);
    if (*map) CloseHandle(*map);
    *hdr = nullptr; *map = nullptr;
}

}  // namespace vbus_win
