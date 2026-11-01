// dyn.h – Auto-Pegel fuer die Aufnahme-Box: Lautheits-Leveler + Look-ahead-True-Peak-Limiter + Messung
// (MIT, © Ben Buchholz · Buccloud Software (DE))
//
// * Leveler: misst die Lautheit nach ITU-R BS.1770 (K-Bewertung; Momentary 400 ms + Short-term 3 s) und regelt die Verstaerkung
//   langsam zum Ziel (dB LUFS): leise Aufnahmen werden angehoben (bis "max. Anhebung"), laute abgesenkt. Unter der Gate-Schwelle
//   (Stille/Rauschen) bleibt die Verstaerkung stehen -> kein Aufpumpen des Rauschens.
// * Limiter: Look-ahead (1,5 ms), True-Peak-Erkennung (4x/2x-Oversampling), geglaettete Verstaerkungs-Hüllkurve (Gleitminimum + Gleitmittel).
//   Garantie: Ausgang <= Decke (true peak), d.h. nie uebersteuert – auch nicht zwischen den Abtastwerten.
// * Messung (immer verfuegbar): Momentary-/Short-term-LUFS, True-Peak, Verstaerkung, Limiter-Reduktion, Clip-Zaehler.
// * Portabel (Windows/Linux) und rein berechnend -> nativ testbar (tests/test_dyn.cpp).
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace dyn {

static const double PI = 3.14159265358979323846;

// ---------- K-Bewertung (BS.1770-4): Hochton-Shelf + RLB-Hochpass, fuer beliebige Abtastrate berechnet
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline double run(double x) {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};
inline void KShelf(double fs, double& b0, double& b1, double& b2, double& a1, double& a2) {
    const double G = 3.999843853973347, f0 = 1681.974450955533, Q = 0.7071752369554196;
    double K = std::tan(PI * f0 / fs), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    b0 = (Vh + Vb * K / Q + K * K) / a0; b1 = 2.0 * (K * K - Vh) / a0; b2 = (Vh - Vb * K / Q + K * K) / a0;
    a1 = 2.0 * (K * K - 1.0) / a0; a2 = (1.0 - K / Q + K * K) / a0;
}
inline void KHighpass(double fs, double& a1, double& a2) {
    const double Q = 0.5003270373253953, fc = 38.13547087613982;
    double K = std::tan(PI * fc / fs), a0 = 1.0 + K / Q + K * K;
    a1 = 2.0 * (K * K - 1.0) / a0; a2 = (1.0 - K / Q + K * K) / a0;
}
struct KWeight {
    Biquad s1, s2;
    void init(double fs) {
        KShelf(fs, s1.b0, s1.b1, s1.b2, s1.a1, s1.a2);
        s2.b0 = 1; s2.b1 = -2; s2.b2 = 1; KHighpass(fs, s2.a1, s2.a2);
        s1.z1 = s1.z2 = s2.z1 = s2.z2 = 0;
    }
    inline double run(double x) { return s2.run(s1.run(x)); }
};

// ---------- Lautheitsmessung (Momentary 400 ms / Short-term 3 s) ueber 100-ms-Bloecke
class LoudMeter {
public:
    void init(int rate) {
        fs_ = rate; blk_ = std::max(1, rate / 10);
        kl_.init(rate); kr_.init(rate); reset();
    }
    void reset() {
        ring_.assign(30, 0.0); n_ = 0; fill_ = 0; acc_ = 0; cnt_ = 0; pos_ = 0;
        kl_.s1.z1 = kl_.s1.z2 = kl_.s2.z1 = kl_.s2.z2 = 0; kr_.s1.z1 = kr_.s1.z2 = kr_.s2.z1 = kr_.s2.z2 = 0;
    }
    inline void push(float l, float r) {
        double a = kl_.run(l), b = kr_.run(r);
        acc_ += a * a + b * b;
        if (++cnt_ >= blk_) {
            ring_[pos_] = acc_ / (double)blk_;
            pos_ = (pos_ + 1) % ring_.size(); if (fill_ < ring_.size()) fill_++;
            acc_ = 0; cnt_ = 0; n_++;
        }
    }
    bool valid() const { return fill_ >= 1; }
    double momentary() const { return lufs(4); }     // LUFS, -120 wenn noch keine Daten
    double shortTerm() const { return lufs(30); }
private:
    double lufs(size_t blocks) const {
        size_t k = std::min(blocks, fill_);
        if (k == 0) return -120.0;
        double s = 0;
        for (size_t i = 0; i < k; i++) s += ring_[(pos_ + ring_.size() - 1 - i) % ring_.size()];
        s /= (double)k;
        return s > 1e-12 ? -0.691 + 10.0 * std::log10(s) : -120.0;
    }
    int fs_ = 48000, blk_ = 4800;
    KWeight kl_, kr_;
    std::vector<double> ring_;
    uint64_t n_ = 0;
    size_t fill_ = 0, pos_ = 0, cnt_ = 0;
    double acc_ = 0;
};

// ---------- True-Peak-Schaetzer (polyphasen-Oversampling: 8x unter 88,2 kHz, 4x unter 176,4 kHz, sonst 2x)
class TruePeak {
public:
    static const int TAPS = 32;   // je Phase
    void init(int rate) {
        R_ = rate < 88200 ? 8 : rate < 176400 ? 4 : 2;   // 8x/4x/2x: auch breitbandige Signale (Rauschen) werden sicher erfasst (Norm verlangt 4x/2x/1x)
        hist_.assign(TAPS, 0.0f); pos_ = 0;
        k_.assign((size_t)R_ * TAPS, 0.0f);
        if (R_ > 1) {   // Kaiser-gefenstertes Sinc (beta 8) als Interpolationsfilter, je Phase auf Summe 1 normiert
            for (int ph = 0; ph < R_; ph++) {
                double frac = (double)ph / R_, sum = 0;
                for (int t = 0; t < TAPS; t++) {
                    double d = (double)(t - TAPS / 2 + 1) - frac;   // Abstand zum Interpolationspunkt (Abtastwerte)
                    double sn = std::fabs(d) < 1e-12 ? 1.0 : std::sin(PI * d) / (PI * d);
                    double r = d / (TAPS / 2);
                    double w = std::fabs(r) >= 1.0 ? 0.0 : I0(8.0 * std::sqrt(1.0 - r * r)) / I0(8.0);
                    k_[(size_t)ph * TAPS + t] = (float)(sn * w); sum += sn * w;
                }
                for (int t = 0; t < TAPS; t++) k_[(size_t)ph * TAPS + t] = (float)(k_[(size_t)ph * TAPS + t] / sum);
            }
        }
    }
    int delay() const { return R_ > 1 ? TAPS / 2 : 0; }   // Verzoegerung der Erkennung in Abtastwerten
    // neuer Abtastwert -> groesster Betrag aller interpolierten Punkte (inkl. des Abtastwerts selbst)
    inline float push(float x) {
        pos_ = (pos_ + 1) % TAPS; hist_[(size_t)pos_] = x;
        float pk = std::fabs(x);
        if (R_ == 1) return pk;
        for (int ph = 1; ph < R_; ph++) {
            float a = 0; const float* c = &k_[(size_t)ph * TAPS];
            for (int t = 0; t < TAPS; t++) a += hist_[(size_t)((pos_ + 1 + t) % TAPS)] * c[t];
            pk = std::max(pk, std::fabs(a));
        }
        // der Abtastwert x liegt am Ende des Fensters; die Interpolierten liegen zwischen den mittleren Werten -> auch deren Abtastwert zaehlt
        float mid = std::fabs(hist_[(size_t)((pos_ + 1 + TAPS / 2 - 1) % TAPS)]);
        return std::max(pk, mid);
    }
    void reset() { std::fill(hist_.begin(), hist_.end(), 0.0f); }
private:
    static double I0(double x) { double s = 1, t = 1; for (int k = 1; k < 60; k++) { double q = x / (2.0 * k); t *= q * q; s += t; if (t < 1e-14 * s) break; } return s; }
    int R_ = 4, pos_ = 0;
    std::vector<float> hist_, k_;
};

// ---------- Einstellungen und Auto-Pegel
struct Params {
    int mode = 0;               // 0 aus, 1 nur Limiter (Schutz vor Uebersteuern), 2 Pegeln (Leveler) + Limiter
    double targetLufs = -18.0;  // Ziel-Lautheit (Short-term), -30 ... -10
    double maxBoostDb = 24.0;   // groesste Anhebung 0 ... 36
    double maxCutDb = 18.0;     // groesste Absenkung 0 ... 36
    double ceilingDb = -1.0;    // Limiter-Decke in dBTP, -12 ... 0
    int speed = 1;              // 0 langsam, 1 normal, 2 schnell
    double gateLufs = -55.0;    // darunter bleibt die Verstaerkung stehen
};

class AutoLevel {
public:
    void init(int rate) {
        fs_ = rate;
        meter_.init(rate);
        for (int c = 0; c < 2; c++) { tp_[c].init(rate); tpo_[c].init(rate); }
        la_ = std::max(8, (int)std::lround(0.0015 * rate));
        dtp_ = tp_[0].delay();
        lm_ = la_ + 3;           // Gleitminimum etwas laenger als der Mittelwert: jeder Mittelwert-Term deckt jeden Peak der Erkennungs-Spreizung ab
        dly_ = la_ + dtp_;       // Audio-Verzoegerung passend zur Erkennungs-Verzoegerung + Mittelwert
        for (int c = 0; c < 2; c++) { buf_[c].assign((size_t)std::max(1, dly_), 0.0f); }
        avgBuf_.assign((size_t)la_, 1.0f);
        reset();
    }
    void reset() {
        for (int c = 0; c < 2; c++) { std::fill(buf_[c].begin(), buf_[c].end(), 0.0f); tp_[c].reset(); tpo_[c].reset(); }
        std::fill(avgBuf_.begin(), avgBuf_.end(), 1.0f);
        minq_.clear(); avgSum_ = (double)la_; avgPos_ = 0; wpos_ = 0; sampleIdx_ = 0;
        limGain_ = 1.0f; levelDb_ = 0.0; meter_.reset();
        gainDb_ = 0; grDb_ = 0; mom_ = st_ = -120; tpMax_ = -120; clips_ = 0; warm_ = 0;
    }
    void set(const Params& p) {
        mode_ = std::max(0, std::min(2, p.mode)); target_ = std::max(-40.0, std::min(-6.0, p.targetLufs));
        boost_ = std::max(0.0, std::min(36.0, p.maxBoostDb)); cut_ = std::max(0.0, std::min(36.0, p.maxCutDb));
        ceil_ = std::max(-24.0, std::min(0.0, p.ceilingDb)); speed_ = std::max(0, std::min(2, p.speed)); gate_ = std::max(-80.0, std::min(-30.0, p.gateLufs));
    }
    int latencyFrames() const { return mode_ == 0 ? 0 : dly_; }

    // stereo interleaved, in place. Bei mode 0 unveraendert (nur Messung).
    void process(float* lr, size_t frames) {
        const int mode = mode_, spd = speed_;
        const double target = target_, boost = boost_, cut = cut_, gate = gate_;
        const double ceilLin = std::pow(10.0, (double)ceil_ / 20.0);
        const double dt = 1.0 / fs_;
        const double sp = spd == 0 ? 0.5 : spd == 2 ? 2.0 : 1.0;
        const double upRate = 2.0 * sp, downRate = 12.0 * sp;   // dB pro Sekunde
        const double relCoef = 1.0 - std::exp(-1.0 / (0.250 * fs_));   // Limiter-Release 250 ms
        float tpPeak = 0, grMax = 0; uint32_t cl = 0;
        for (size_t i = 0; i < frames; i++) {
            float l = lr[2 * i], r = lr[2 * i + 1];
            if (!(l == l)) l = 0; if (!(r == r)) r = 0;
            meter_.push(l, r);   // gemessen wird das EINGANGSsignal (vor Verstaerkung)
            if (mode == 0) { lr[2 * i] = l; lr[2 * i + 1] = r; continue; }
            // --- Leveler (alle 1 ms Zielwert nachfuehren)
            if ((sampleIdx_ & 63) == 0) {
                double mo = meter_.momentary(), sh = meter_.shortTerm(); mom_ = mo; st_ = sh;
                if (mode == 2 && meter_.valid()) {
                    double est = std::max(mo, sh);
                    if (mo > gate) {   // nur nachregeln, solange JETZT Signal anliegt (Pausen/Stille: Verstaerkung bleibt stehen)
                        double want = std::max(-cut, std::min(boost, target - est));
                        double step = 64.0 * dt;
                        if (want < levelDb_) levelDb_ = std::max(want, levelDb_ - downRate * step);
                        else levelDb_ = std::min(want, levelDb_ + upRate * step);
                    }
                } else if (mode == 1) levelDb_ = 0.0;
                levelDb_ = std::max(-cut, std::min(boost, levelDb_));
            }
            sampleIdx_++;
            const float g = (float)std::pow(10.0, levelDb_ / 20.0);
            float xl = l * g, xr = r * g;
            // --- Limiter: Erkennung (True Peak) auf dem unverzoegerten Signal
            float pk = std::max(tp_[0].push(xl), tp_[1].push(xr));
            float req = pk > (float)ceilLin ? (float)ceilLin / pk : 1.0f;
            // Gleitminimum ueber la_ Werte (monotone Warteschlange)
            while (!minq_.empty() && minq_.back().second >= req) minq_.pop_back();
            minq_.push_back({sampleIdx_, req});
            while (minq_.front().first + (uint64_t)lm_ <= sampleIdx_) minq_.pop_front();
            float m = minq_.front().second;
            // Gleitmittel ueber la_ Werte
            avgSum_ += (double)m - (double)avgBuf_[(size_t)avgPos_]; avgBuf_[(size_t)avgPos_] = m; avgPos_ = (avgPos_ + 1) % la_;
            float tgt = (float)(avgSum_ / la_);
            if (tgt < limGain_) limGain_ = tgt; else limGain_ += (tgt - limGain_) * (float)relCoef;
            // --- Verzoegerung des Audiosignals
            size_t N = buf_[0].size();
            float dl = buf_[0][wpos_], dr2 = buf_[1][wpos_];
            buf_[0][wpos_] = xl; buf_[1][wpos_] = xr; wpos_ = (wpos_ + 1) % N;
            float ol = dl * limGain_, orr = dr2 * limGain_;
            // letzte Sicherung: nie ueber die Decke (Abtastwert)
            float c = (float)ceilLin;
            if (std::fabs(ol) > c || std::fabs(orr) > c) clamps_++;
            if (ol > c) ol = c; else if (ol < -c) ol = -c; if (orr > c) orr = c; else if (orr < -c) orr = -c;
            lr[2 * i] = ol; lr[2 * i + 1] = orr;
            tpPeak = std::max(tpPeak, std::max(tpo_[0].push(ol), tpo_[1].push(orr))); grMax = std::max(grMax, 1.0f - limGain_);
            if (std::fabs(ol) >= 0.9999f || std::fabs(orr) >= 0.9999f) cl++;
        }
        gainDb_ = (float)levelDb_;
        grDb_ = grMax > 0 ? (float)(-20.0 * std::log10(std::max(1e-6f, 1.0f - grMax))) : 0.0f;
        double tdb = tpPeak > 1e-6f ? 20.0 * std::log10(tpPeak) : -120.0;
        if (tdb > tpMax_) tpMax_ = (float)tdb;
        clips_ += cl;
        if (mode == 0) { mom_ = meter_.momentary(); st_ = meter_.shortTerm(); }
    }
    // Messwerte (fuer Oberflaeche / Web)
    float gainDb() const { return gainDb_; }              // aktuelle Leveler-Verstaerkung
    float limiterDb() const { return grDb_; }             // staerkste Limiter-Absenkung im letzten Block (dB, positiv)
    float momentaryLufs() const { return (float)mom_; }
    float shortLufs() const { return (float)st_; }
    float truePeakDb() const { return tpMax_; }           // groesster True Peak seit Reset (nach Verarbeitung)
    uint32_t clips() const { return clips_; }
    uint32_t clamps() const { return clamps_; }          // Sicherheits-Begrenzungen (sollte 0 bleiben)
    void resetPeaks() { tpMax_ = -120; clips_ = 0; }
    int mode() const { return mode_; }
private:
    int fs_ = 48000, la_ = 72, lm_ = 75, dly_ = 80, dtp_ = 8;
    std::atomic<int> mode_{0}, speed_{1};
    std::atomic<double> target_{-18.0}, boost_{24.0}, cut_{18.0}, ceil_{-1.0}, gate_{-55.0};
    LoudMeter meter_;
    TruePeak tp_[2], tpo_[2];
    std::vector<float> buf_[2], avgBuf_;
    std::deque<std::pair<uint64_t, float>> minq_;
    double avgSum_ = 0, levelDb_ = 0;
    int avgPos_ = 0; size_t wpos_ = 0; uint64_t sampleIdx_ = 0;
    float limGain_ = 1.0f;
    std::atomic<float> gainDb_{0}, grDb_{0}, tpMax_{-120};
    std::atomic<double> mom_{-120}, st_{-120};
    std::atomic<uint32_t> clips_{0}, clamps_{0};
    int warm_ = 0;
};

}  // namespace dyn
