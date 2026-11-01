// wavrec.h – Rekorder der Aufnahme-Box: WAV (bis 16 Kanaele) oder FLAC (MIT, © Ben Buchholz · Buccloud Software (DE))
//
// * Audio-Thread ruft nur push() bzw. monitor() auf (kopiert in einen Ringpuffer, blockiert nie).
// * Ein eigener Schreib-Thread schreibt auf die Platte -> Festplatten-Hakler stoeren das Audio nicht. Der Ringpuffer waechst mit
//   Datenrate und Kanalzahl (mind. 8 s), damit auch 16 Kanaele bei 192 kHz sicher durchlaufen.
// * WAV: 16 Bit PCM (TPDF-Dither), 24 Bit PCM, 32 Bit Ganzzahl-PCM oder 32 Bit Float; 1 bis 16 Kanaele (WAVE_FORMAT_EXTENSIBLE mit Kanalmaske);
//   Abtastraten frei (44,1 ... 384 kHz). Bit-genau: 24-/32-Bit-Quellen laufen ohne Skalierungsfehler durch (Faktor 2^(n-1)).
//   Ab 4 GiB stellt der Header automatisch auf RF64 um. Broadcast-Wave-Zeitstempel ("bext", Zeitreferenz) fuer die Platzierung in der DAW.
// * FLAC: 16 oder 24 Bit Stereo, verlustfrei (eigener Encoder, flac.h), etwa halb so gross wie WAV.
// * Optional Dateiteilung (nach Dauer oder Groesse, luckenlos) und Vorlauf-Puffer ("Pre-Record": die letzten Sekunden VOR dem Start-Druck).
// * Der Header wird waehrend der Aufnahme regelmaessig aktualisiert: nach Absturz/Stromausfall bleibt die Datei lesbar.
// * Portabel (Windows/Linux), daher mit tests/test_wavrec.cpp nativ testbar.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "flac.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace wavrec {

inline uint64_t& Rf64Threshold() { static uint64_t v = 0xFFFFF000ull; return v; }   // Nutzdaten-Bytes, ab denen WAV auf RF64 umstellt (nur fuer Tests aenderbar)
inline double& HeaderInterval() { static double v = 1.0; return v; }                 // Sekunden
static const int FLAC_BLOCK = 4096;
static const int MAX_CH = 16;

struct Options {
    std::string path;              // Zieldatei (UTF-8); bei Teilung kommt vor der Endung "_002", "_003" ...
    int rate = 48000;
    int bits = 24;                 // 16, 24, 32
    bool flac = false;             // nur Stereo, 16/24 Bit
    bool int32 = false;            // bei bits == 32: Ganzzahl-PCM statt Float
    int channels = 2;              // 1 ... 16 (WAV)
    int dither = -1;               // -1 automatisch (TPDF bei 16 Bit), 0 aus, 1 TPDF (16/24 Bit)
    double splitSeconds = 0;       // > 0: neue Datei nach so vielen Sekunden
    uint64_t splitBytes = 0;       // > 0: neue Datei nach so vielen Nutzdaten-Bytes
    std::string description;       // BWF-Beschreibung (max. 256 Zeichen)
    uint64_t timeRefSamples = 0;   // BWF-Zeitreferenz: Abtastwerte seit Mitternacht beim ersten Frame
};

class Recorder {
public:
    ~Recorder() { stop(); }

    bool start(const Options& o, std::string& err) {
        stop();
        if (o.rate < 8000 || o.rate > 768000) { err = "Ungueltige Abtastrate"; return false; }
        if (o.channels < 1 || o.channels > MAX_CH) { err = "Kanalzahl 1 bis 16"; return false; }
        if (o.flac) {
            if (o.bits != 16 && o.bits != 24) { err = "FLAC: 16 oder 24 Bit"; return false; }
            if (o.channels != 2) { err = "FLAC kennt hier nur Stereo - fuer mehr Kanaele WAV waehlen"; return false; }
        } else if (o.bits != 16 && o.bits != 24 && o.bits != 32) { err = "Ungueltige Bittiefe"; return false; }
        rate_ = o.rate; bits_ = o.bits; flac_ = o.flac; ch_ = o.channels;
        float32_ = (!o.flac && o.bits == 32 && !o.int32);
        block_ = ch_ * (bits_ / 8);
        dither_ = o.dither < 0 ? (bits_ == 16 ? 1 : 0) : (o.dither ? 1 : 0);
        if (bits_ == 32) dither_ = 0;
        splitFrames_ = o.splitSeconds > 0 ? (uint64_t)std::llround(o.splitSeconds * o.rate) : 0;
        splitBytes_ = o.splitBytes;
        desc_ = o.description.substr(0, 256); timeRef_ = o.timeRefSamples;
        base_ = o.path; part_ = 1;
        if (!openFile(base_, err)) return false;
        // Ringpuffer: mindestens 16 MiB, mindestens 8 s Datenrate (Zweierpotenz)
        size_t want = std::max<size_t>(16u << 20, (size_t)((double)o.rate * block_ * 8.0)), sz = 1; while (sz < want) sz <<= 1;
        if (ring_.size() != sz) ring_.assign(sz, 0);
        wr_ = 0; rd_ = 0; frames_ = 0; bytesTotal_ = 0; overruns_ = 0; failed_ = false; stopFlag_ = false;
        minFr_ = 0; maxFr_ = 0; frameNo_ = 0; partFrames_ = 0; parts_ = 1;
        { std::lock_guard<std::mutex> lk(mu_); lastErr_.clear(); cur_ = base_; }
        rng_ = 0x2545F491u;
        seedPreRoll();   // Vorlauf zuerst (nur wenn Kanalzahl/Rate passen)
        thr_ = std::thread([this] { loop(); });
        active_.store(true, std::memory_order_release);
        return true;
    }
    bool start(const std::string& pathUtf8, int rate, int bits, bool useFlac, std::string& err) {
        Options o; o.path = pathUtf8; o.rate = rate; o.bits = bits; o.flac = useFlac; return start(o, err);
    }
    bool start(const std::string& pathUtf8, int rate, int bits, std::string& err) { return start(pathUtf8, rate, bits, false, err); }

    void stop() {
        if (!thr_.joinable()) { active_ = false; return; }
        active_.store(false, std::memory_order_release);
        { std::lock_guard<std::mutex> lk(mu_); stopFlag_ = true; }
        cv_.notify_all();
        thr_.join();
    }

    bool active() const { return active_.load(std::memory_order_acquire); }
    bool failed() const { return failed_.load(); }
    uint64_t frames() const { return frames_.load(); }           // aufgenommene Frames (alle Teile)
    uint64_t bytes() const { return bytesTotal_.load(); }        // geschriebene Bytes (Nutzdaten bzw. FLAC-Daten)
    uint32_t overruns() const { return overruns_.load(); }       // verworfene Bloecke (Platte zu langsam)
    uint32_t parts() const { return parts_.load(); }             // Anzahl Dateien (Teilung)
    int rate() const { return rate_; }
    int channels() const { return ch_; }
    double ringFill() const { return ring_.empty() ? 0.0 : (double)(wr_.load() - rd_.load()) / (double)ring_.size(); }   // 0 ... 1 (Platten-Auslastung)
    std::string file() { std::lock_guard<std::mutex> lk(mu_); return cur_; }
    std::string lastError() { std::lock_guard<std::mutex> lk(mu_); return lastErr_; }

    // ----- Vorlauf-Puffer ("Pre-Record"): haelt die letzten 'seconds' Sekunden bereit, solange nicht aufgenommen wird
    void arm(int rate, int channels, double seconds) {
        std::lock_guard<std::mutex> lk(preMu_);
        if (seconds <= 0 || channels < 1 || channels > MAX_CH) { preFrames_ = 0; preBuf_.clear(); preBuf_.shrink_to_fit(); preCount_ = 0; return; }
        double maxFr = 256.0 * 1024 * 1024 / 4.0 / channels;   // hoechstens 256 MiB
        size_t fr = (size_t)std::min(seconds * rate, maxFr);
        if (preRate_ == rate && preCh_ == channels && preFrames_ == fr) return;
        preRate_ = rate; preCh_ = channels; preFrames_ = fr; preBuf_.assign(fr * (size_t)channels, 0.0f); preCount_ = 0; prePos_ = 0;
    }
    double armedSeconds() const { return preFrames_ && preRate_ ? (double)std::min<uint64_t>(preCount_, preFrames_) / preRate_ : 0.0; }

    // Audio-Thread: laufende Aufnahme -> push(); sonst Vorlauf-Puffer nachfuehren
    void monitor(const float* il, size_t frames) {
        if (active_.load(std::memory_order_acquire)) { push(il, frames); return; }
        if (preFrames_ == 0 || !preMu_.try_lock()) return;
        const size_t C = (size_t)preCh_;
        for (size_t i = 0; i < frames; i++) { std::memcpy(&preBuf_[prePos_ * C], il + i * C, C * sizeof(float)); prePos_ = (prePos_ + 1) % preFrames_; }
        preCount_ += frames; preMu_.unlock();
    }

    // Audio-Thread: interleaved Float mit 'channels' Kanaelen. Nie blockierend.
    void push(const float* il, size_t frames) {
        if (!active_.load(std::memory_order_acquire) || frames == 0) return;
        pushRaw(il, frames);
    }

private:
    void pushRaw(const float* il, size_t frames) {
        const size_t need = frames * (size_t)block_;
        const uint64_t w = wr_.load(std::memory_order_relaxed), r = rd_.load(std::memory_order_acquire);
        if (ring_.size() - (size_t)(w - r) < need) { overruns_++; return; }
        const size_t mask = ring_.size() - 1;
        size_t pos = (size_t)(w & mask);
        auto put = [&](const uint8_t* p, size_t n) {   // 'n' Bytes mit Umbruch
            size_t first = std::min(n, ring_.size() - pos);
            std::memcpy(&ring_[pos], p, first);
            if (first < n) std::memcpy(&ring_[0], p + first, n - first);
            pos = (pos + n) & mask;
        };
        uint8_t tmp[8192]; size_t t = 0;
        const size_t total = frames * (size_t)ch_;
        for (size_t i = 0; i < total; i++) {
            float x = il[i]; if (!(x == x)) x = 0.0f;   // NaN -> 0
            if (float32_) { std::memcpy(tmp + t, &x, 4); t += 4; }
            else if (bits_ == 32) {   // Ganzzahl 32 Bit, Faktor 2^31 (bit-genau fuer 24-Bit-Quellen)
                double d = (double)x * 2147483648.0; d = std::max(-2147483648.0, std::min(2147483647.0, d));
                int32_t v = (int32_t)std::llrint(d);
                std::memcpy(tmp + t, &v, 4); t += 4;
            } else if (bits_ == 24) {   // Faktor 2^23: 24-Bit-Quellen kommen ohne jeden Fehler wieder heraus
                float f = x * 8388608.0f;
                if (dither_) f += nextU() + nextU() - 1.0f;
                int32_t v = (int32_t)std::lrintf(f);
                v = std::max(-8388608, std::min(8388607, v));
                tmp[t++] = (uint8_t)v; tmp[t++] = (uint8_t)(v >> 8); tmp[t++] = (uint8_t)(v >> 16);
            } else {
                float f = x * 32768.0f;
                if (dither_) f += nextU() + nextU() - 1.0f;   // TPDF-Dither, +-1 LSB
                int32_t v = (int32_t)std::floor(f + 0.5f);
                v = std::max(-32768, std::min(32767, v));
                tmp[t++] = (uint8_t)v; tmp[t++] = (uint8_t)(v >> 8);
            }
            if (t > sizeof(tmp) - 8) { put(tmp, t); t = 0; }
        }
        if (t) put(tmp, t);
        wr_.store(w + need, std::memory_order_release);
        cv_.notify_one();
    }
    void seedPreRoll() {
        std::lock_guard<std::mutex> lk(preMu_);
        if (preFrames_ == 0 || preCh_ != ch_ || preRate_ != rate_ || preCount_ == 0) return;
        size_t n = (size_t)std::min<uint64_t>(preCount_, preFrames_), startPos = (prePos_ + preFrames_ - n) % preFrames_;
        const size_t C = (size_t)ch_; std::vector<float> tmp(4096 * C);
        for (size_t done = 0; done < n;) {
            size_t k = std::min<size_t>(4096, n - done);
            for (size_t i = 0; i < k; i++) std::memcpy(&tmp[i * C], &preBuf_[((startPos + done + i) % preFrames_) * C], C * sizeof(float));
            pushRaw(tmp.data(), k); done += k;
        }
        preCount_ = 0;   // einmal verbraucht
    }

    // ----- Datei
    static std::FILE* Open(const std::string& u8) {
#ifdef _WIN32
        int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, nullptr, 0);
        std::wstring w((size_t)std::max(1, n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, &w[0], n);
        return _wfopen(w.c_str(), L"wb");
#else
        return std::fopen(u8.c_str(), "wb");
#endif
    }
    static void Seek(std::FILE* f, long long off, int whence) {
#ifdef _WIN32
        _fseeki64(f, off, whence);
#else
        fseeko(f, (off_t)off, whence);
#endif
    }
    static void W32(std::FILE* f, uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; std::fwrite(b, 1, 4, f); }
    static void W64(std::FILE* f, uint64_t v) { W32(f, (uint32_t)v); W32(f, (uint32_t)(v >> 32)); }
    static void W16(std::FILE* f, uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; std::fwrite(b, 1, 2, f); }
    static uint32_t DefaultMask(int ch) {   // Standard-Kanalmasken (WAVEFORMATEXTENSIBLE)
        switch (ch) { case 1: return 0x4; case 2: return 0x3; case 4: return 0x33; case 6: return 0x3F; case 8: return 0x63F; default: return 0; }
    }
    std::string partPath(int part) const {
        if (part <= 1) return base_;
        size_t dot = base_.find_last_of('.'), sl = base_.find_last_of("/\\");
        if (dot == std::string::npos || (sl != std::string::npos && dot < sl)) dot = base_.size();
        char sfx[16]; std::snprintf(sfx, sizeof sfx, "_%03d", part);
        return base_.substr(0, dot) + sfx + base_.substr(dot);
    }

    void writeBext(std::FILE* f) {   // Broadcast-Wave-Chunk "bext" (602 Bytes, ohne Coding History)
        std::fwrite("bext", 1, 4, f); W32(f, 602);
        char b[602]; std::memset(b, 0, sizeof b);
        std::snprintf(b, 256, "%s", desc_.empty() ? "Bens Audio Router" : desc_.c_str());   // Description (256)
        std::snprintf(b + 256, 32, "BensAudioRouter");                                      // Originator (32)
        std::snprintf(b + 288, 32, "BAR-%llu", (unsigned long long)std::time(nullptr));     // OriginatorReference (32)
        std::time_t now = std::time(nullptr); std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        char dt[40], tm2[40];
        std::snprintf(dt, sizeof dt, "%04d-%02d-%02d", (tmv.tm_year + 1900) % 10000, (tmv.tm_mon + 1) % 100, tmv.tm_mday % 100); std::memcpy(b + 320, dt, 10);   // OriginationDate (10)
        std::snprintf(tm2, sizeof tm2, "%02d:%02d:%02d", tmv.tm_hour % 100, tmv.tm_min % 100, tmv.tm_sec % 100); std::memcpy(b + 330, tm2, 8);                  // OriginationTime (8)
        uint64_t tr = timeRef_; std::memcpy(b + 338, &tr, 8);                                             // TimeReference (Low/High)
        uint16_t ver = 1; std::memcpy(b + 346, &ver, 2);                                                  // Version
        std::fwrite(b, 1, 602, f);
    }
    void writeWavHeader(std::FILE* f, uint64_t dataBytes) {
        const bool ext = bits_ != 16 || ch_ > 2, flt = float32_;
        const uint32_t fmtLen = ext ? 40 : 16;
        hdr_ = 12 + 36 + (8 + 602) + 8 + fmtLen + (flt ? 12 : 0) + 8;   // 36 = JUNK-Platzhalter (wird bei >4 GiB zu "ds64")
        std::fwrite("RIFF", 1, 4, f); W32(f, (uint32_t)(hdr_ - 8 + dataBytes)); std::fwrite("WAVE", 1, 4, f);
        std::fwrite("JUNK", 1, 4, f); W32(f, 28); { uint8_t z[28] = {0}; std::fwrite(z, 1, 28, f); }
        writeBext(f);
        std::fwrite("fmt ", 1, 4, f); W32(f, fmtLen);
        W16(f, ext ? 0xFFFE : 1); W16(f, (uint16_t)ch_); W32(f, (uint32_t)rate_); W32(f, (uint32_t)((uint64_t)rate_ * (uint64_t)block_)); W16(f, (uint16_t)block_); W16(f, (uint16_t)bits_);
        if (ext) {
            W16(f, 22); W16(f, (uint16_t)bits_); W32(f, DefaultMask(ch_));   // cbSize, gueltige Bits, Kanalmaske
            static const uint8_t guid[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
            W32(f, flt ? 3 : 1); std::fwrite(guid, 1, 12, f);
        }
        if (flt) { std::fwrite("fact", 1, 4, f); W32(f, 4); W32(f, (uint32_t)(dataBytes / (uint64_t)block_)); }
        std::fwrite("data", 1, 4, f); W32(f, (uint32_t)dataBytes);
    }
    void fixHeader() {   // Groessenfelder nachtragen (Aufnahme laeuft weiter)
        std::fflush(f_);
        if (flac_) {   // STREAMINFO: Frame-Groessen (Byte 4-9) und Rate/Kanaele/Bits/Gesamtsamples (Byte 10-17)
            Seek(f_, 8 + 4, SEEK_SET);
            uint8_t si[14];
            si[0] = (uint8_t)(minFr_ >> 16); si[1] = (uint8_t)(minFr_ >> 8); si[2] = (uint8_t)minFr_;
            si[3] = (uint8_t)(maxFr_ >> 16); si[4] = (uint8_t)(maxFr_ >> 8); si[5] = (uint8_t)maxFr_;
            uint64_t v = ((uint64_t)rate_ << 44) | ((uint64_t)1 << 41) | ((uint64_t)(bits_ - 1) << 36) | (partFrames_ & 0xFFFFFFFFFull);
            for (int i = 0; i < 8; i++) si[6 + i] = (uint8_t)(v >> (56 - 8 * i));
            std::fwrite(si, 1, 14, f_);
        } else {
            const uint64_t frames = data_ / (uint64_t)block_;
            if (data_ <= Rf64Threshold()) {   // klassisches RIFF
                Seek(f_, 4, SEEK_SET); W32(f_, (uint32_t)(hdr_ - 8 + data_));
                if (float32_) { Seek(f_, hdr_ - 8 - 4, SEEK_SET); W32(f_, (uint32_t)frames); }
                Seek(f_, hdr_ - 4, SEEK_SET); W32(f_, (uint32_t)data_);
            } else {                          // RF64: "RF64" + ds64 (aus dem JUNK-Platzhalter), 32-Bit-Felder = 0xFFFFFFFF
                Seek(f_, 0, SEEK_SET); std::fwrite("RF64", 1, 4, f_); W32(f_, 0xFFFFFFFFu);
                Seek(f_, 12, SEEK_SET); std::fwrite("ds64", 1, 4, f_); W32(f_, 28);
                W64(f_, hdr_ - 8 + data_); W64(f_, data_); W64(f_, frames); W32(f_, 0);
                if (float32_) { Seek(f_, hdr_ - 8 - 4, SEEK_SET); W32(f_, 0xFFFFFFFFu); }
                Seek(f_, hdr_ - 4, SEEK_SET); W32(f_, 0xFFFFFFFFu);
            }
        }
        std::fflush(f_);
        Seek(f_, 0, SEEK_END);
    }
    bool openFile(const std::string& path, std::string& err) {
        f_ = Open(path);
        if (!f_) { err = "Datei kann nicht angelegt werden: " + path; return false; }
        data_ = 0; partFrames_ = 0; minFr_ = 0; maxFr_ = 0; frameNo_ = 0;
        if (flac_) { std::vector<uint8_t> h = flac::StreamHeader(rate_, bits_, FLAC_BLOCK); std::fwrite(h.data(), 1, h.size(), f_); }
        else writeWavHeader(f_, 0);
        return true;
    }
    void closeFile() {
        if (!f_) return;
        fixHeader(); std::fclose(f_); f_ = nullptr;
    }
    bool nextPart() {   // luckenlos: alte Datei abschliessen, neue beginnen (der Ringpuffer laeuft weiter)
        closeFile(); part_++;
        std::string p = partPath(part_), err;
        timeRef_ += partFramesLast_;   // BWF-Zeitreferenz der neuen Datei (vor dem Header schreiben)
        if (!openFile(p, err)) { fail(err); return false; }
        parts_ = (uint32_t)part_;
        { std::lock_guard<std::mutex> lk(mu_); cur_ = p; }
        return true;
    }
    void fail(const std::string& m) {
        { std::lock_guard<std::mutex> lk(mu_); lastErr_ = m; }
        failed_ = true; active_.store(false, std::memory_order_release);
    }
    bool partFull(uint64_t incomingFrames = 0) const {
        if (splitFrames_ && partFrames_ + incomingFrames > splitFrames_) return true;
        if (splitBytes_ && data_ + incomingFrames * (uint64_t)block_ > splitBytes_) return true;
        return false;
    }
    size_t roomFrames() const {   // wie viele Frames passen noch in die aktuelle Datei (Teilung)
        uint64_t r = ~0ull;
        if (splitFrames_) r = std::min<uint64_t>(r, splitFrames_ > partFrames_ ? splitFrames_ - partFrames_ : 0);
        if (splitBytes_) r = std::min<uint64_t>(r, splitBytes_ > data_ ? (splitBytes_ - data_) / (uint64_t)block_ : 0);
        return r > (1ull << 40) ? (size_t)(1ull << 40) : (size_t)r;
    }

    // ----- Schreib-Thread
    void readRing(uint64_t r, size_t n, uint8_t* dst) const {
        const size_t mask = ring_.size() - 1, pos = (size_t)(r & mask), first = std::min(n, ring_.size() - pos);
        std::memcpy(dst, &ring_[pos], first);
        if (first < n) std::memcpy(dst + first, &ring_[0], n - first);
    }
    bool writeFlacBlock(const uint8_t* p, size_t N) {
        fl_.resize(N); fr_.resize(N);
        const size_t B = (size_t)bits_ / 8;
        for (size_t i = 0; i < N; i++) {
            for (int c = 0; c < 2; c++) {
                const uint8_t* q = p + (i * 2 + (size_t)c) * B;
                int32_t v = B == 3 ? (int32_t)(((uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16)) << 8) >> 8 : (int32_t)(int16_t)((uint32_t)q[0] | ((uint32_t)q[1] << 8));
                (c ? fr_ : fl_)[i] = v;
            }
        }
        enc_.clear();
        flac::EncodeFrame(enc_, fl_.data(), fr_.data(), (int)N, bits_, rate_, frameNo_++);
        if (std::fwrite(enc_.data(), 1, enc_.size(), f_) != enc_.size()) return false;
        const uint32_t sz = (uint32_t)enc_.size();
        if (!minFr_ || sz < minFr_) minFr_ = sz;
        if (sz > maxFr_) maxFr_ = sz;
        data_ += sz; bytesTotal_ += sz; frames_ += N; partFrames_ += N; partFramesLast_ = partFrames_;
        return true;
    }
    void loop() {
        std::vector<uint8_t> chunk(256 * 1024);
        auto lastFix = std::chrono::steady_clock::now();
        const bool split = splitFrames_ || splitBytes_;
        for (;;) {
            const uint64_t r = rd_.load(std::memory_order_relaxed), w = wr_.load(std::memory_order_acquire);
            const size_t avail = (size_t)(w - r);
            bool stopping; { std::lock_guard<std::mutex> lk(mu_); stopping = stopFlag_; }
            if (flac_) {
                const size_t fb = (size_t)FLAC_BLOCK * (size_t)block_;
                if (avail >= fb || (stopping && avail > 0)) {
                    size_t nb = std::min(avail, fb); nb -= nb % (size_t)block_;
                    if (split) { if (partFull()) { if (!nextPart()) break; } size_t room = roomFrames(); if (room < nb / (size_t)block_) nb = room * (size_t)block_; if (nb == 0) { if (!nextPart()) break; continue; } }
                    if (chunk.size() < nb) chunk.resize(nb);
                    readRing(r, nb, chunk.data());
                    if (!writeFlacBlock(chunk.data(), nb / (size_t)block_)) { fail("Schreibfehler (Datentraeger voll?)"); break; }
                    rd_.store(r + nb, std::memory_order_release);
                } else if (stopping) break;
                else { std::unique_lock<std::mutex> lk(mu_); cv_.wait_for(lk, std::chrono::milliseconds(20)); }
            } else {
                if (avail == 0) {
                    if (stopping) break;
                    std::unique_lock<std::mutex> lk(mu_); cv_.wait_for(lk, std::chrono::milliseconds(20));
                } else {
                    size_t n = std::min(avail, chunk.size()); n -= n % (size_t)block_;
                    if (split) {
                        if (partFull()) { if (!nextPart()) break; }
                        size_t room = roomFrames(); if (room < n / (size_t)block_) n = room * (size_t)block_;
                        if (n == 0) { if (!nextPart()) break; continue; }
                    }
                    readRing(r, n, chunk.data());
                    if (std::fwrite(chunk.data(), 1, n, f_) != n) { fail("Schreibfehler (Datentraeger voll?)"); break; }
                    data_ += n; bytesTotal_ += n; frames_ += n / (size_t)block_; partFrames_ += n / (size_t)block_; partFramesLast_ = partFrames_;
                    rd_.store(r + n, std::memory_order_release);
                }
            }
            auto now = std::chrono::steady_clock::now();
            if (f_ && std::chrono::duration<double>(now - lastFix).count() >= HeaderInterval()) { fixHeader(); lastFix = now; }
        }
        closeFile();
    }

    float nextU() { rng_ = rng_ * 1664525u + 1013904223u; return (float)(rng_ >> 8) * (1.0f / 16777216.0f); }

    std::FILE* f_ = nullptr;
    std::string cur_, lastErr_, base_, desc_;
    int rate_ = 48000, bits_ = 24, block_ = 6, ch_ = 2, part_ = 1, dither_ = 0;
    bool flac_ = false, float32_ = false;
    uint32_t hdr_ = 44, rng_ = 1, minFr_ = 0, maxFr_ = 0;
    uint64_t data_ = 0, frameNo_ = 0, partFrames_ = 0, partFramesLast_ = 0, splitFrames_ = 0, splitBytes_ = 0, timeRef_ = 0;
    std::vector<uint8_t> ring_, enc_;
    std::vector<int32_t> fl_, fr_;
    std::atomic<uint64_t> wr_{0}, rd_{0}, frames_{0}, bytesTotal_{0};
    std::atomic<uint32_t> overruns_{0}, parts_{1};
    std::atomic<bool> active_{false}, failed_{false};
    bool stopFlag_ = false;
    std::mutex mu_, preMu_;
    std::condition_variable cv_;
    std::thread thr_;
    // Vorlauf
    std::vector<float> preBuf_;
    size_t preFrames_ = 0, prePos_ = 0; uint64_t preCount_ = 0; int preRate_ = 0, preCh_ = 0;
};

}  // namespace wavrec
