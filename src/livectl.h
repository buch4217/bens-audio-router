// livectl.h – Live-Modus: adaptive Router-Latenz ("so klein wie stabil moeglich")
// (MIT, © Ben Buchholz · Buccloud Software (DE))
//
// * Start bei 'startMs'. Treten Unterlaeufe (Ausgabe-Puffer leer) oder Neu-Synchronisierungen der Quellen auf, wird der Puffer um 'stepUp' ms
//   groesser (hoechstens 'maxMs'); danach mindestens 'holdSec' Ruhe, bevor erneut erhoeht wird.
// * Bleibt es 'calmSec' lang ruhig, wird der Puffer in kleinen Schritten ('stepDown') wieder abgesenkt - nie unter 'minMs' und nie unter die
//   Marke, bei der zuletzt Aussetzer auftraten ('floorMs', sinkt nach 4 x calmSec wieder).
// * Portabel und rein rechnend -> nativ testbar (tests/test_livectl.cpp).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>

class LiveCtl {
public:
    struct Cfg { double minMs = 2.0, maxMs = 40.0, startMs = 8.0, stepUp = 2.0, stepDown = 0.5, holdSec = 3.0, calmSec = 45.0; };
    void init(const Cfg& c) { cfg_ = c; lag_ = std::min(c.maxMs, std::max(c.minMs, c.startMs)); floor_ = c.minMs; lastEv_ = lastRaise_ = lastLower_ = lastFloorDecay_ = -1e9; seenU_ = seenR_ = 0; first_ = true; raises_ = 0; }
    void setAuto(bool a) { auto_ = a; }
    bool isAuto() const { return auto_; }
    void setManual(double ms) { lag_ = std::min(cfg_.maxMs, std::max(cfg_.minMs, ms)); }
    double lagMs() const { return lag_.load(); }
    uint32_t raises() const { return raises_; }       // wie oft wurde erhoeht (Anzeige)
    double floorMs() const { return floor_; }
    // alle ~100 ms aufrufen: kumulierte Zaehler der Engine
    void tick(double now, uint32_t underruns, uint32_t resyncs) {
        if (first_) { seenU_ = underruns; seenR_ = resyncs; first_ = false; lastEv_ = now; lastLower_ = now; lastFloorDecay_ = now; return; }
        const bool ev = underruns != seenU_ || resyncs != seenR_; seenU_ = underruns; seenR_ = resyncs;
        if (!auto_) return;
        double lag = lag_;
        if (ev) {
            lastEv_ = now;
            if (now - lastRaise_ >= cfg_.holdSec) { floor_ = std::min(cfg_.maxMs, lag + cfg_.stepUp * 0.5); lag = std::min(cfg_.maxMs, lag + cfg_.stepUp); lastRaise_ = now; raises_++; }
        } else if (now - lastEv_ >= cfg_.calmSec && now - lastLower_ >= cfg_.calmSec / 3.0) {
            double lo = std::max(cfg_.minMs, floor_); if (lag > lo) lag = std::max(lo, lag - cfg_.stepDown); lastLower_ = now;
        }
        if (floor_ > cfg_.minMs && now - lastEv_ >= 4.0 * cfg_.calmSec && now - lastFloorDecay_ >= cfg_.calmSec) { floor_ = std::max(cfg_.minMs, floor_ - cfg_.stepDown); lastFloorDecay_ = now; }
        lag_ = lag;
    }
private:
    Cfg cfg_; std::atomic<double> lag_{8.0}; double floor_ = 2.0, lastEv_ = 0, lastRaise_ = 0, lastLower_ = 0, lastFloorDecay_ = 0; uint32_t seenU_ = 0, seenR_ = 0, raises_ = 0; bool first_ = true; std::atomic<bool> auto_{true};
};
