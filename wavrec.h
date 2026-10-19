// wavrec.h – Rekorder der Aufnahme-Box: WAV oder FLAC (MIT, © Ben Buchholz · Buccloud Software (DE))
//
// * Audio-Thread ruft nur push() auf (kopiert in einen Ringpuffer, blockiert nie).
// * Ein eigener Schreib-Thread schreibt auf die Platte -> Festplatten-Hakler stoeren das Audio nicht.
// * WAV: 16 Bit PCM (TPDF-Dither), 24 Bit PCM oder 32 Bit Float (24/32 Bit als WAVE_FORMAT_EXTENSIBLE).
//   Ueber 4 GiB stellt der Header automatisch auf RF64 um (WAV mit 64-Bit-Groessen).
// * FLAC: 16 oder 24 Bit, verlustfrei (eigener Encoder, flac.h), etwa halb so gross wie WAV, keine 4-GiB-Grenze.
// * Immer genau EINE Datei pro Aufnahme, keine Teilung.
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

class Recorder {
public:
    ~Recorder() { stop(); }

    // bits: WAV 16/24/32(Float); FLAC 16/24. Rueckgabe false + err bei Fehler.
    bool start(const std::string& pathUtf8, int rate, int bits, bool useFlac, std::string& err) {
        stop();
        if (useFlac ? (bits != 16 && bits != 24) : (bits != 16 && bits != 24 && bits != 32)) { err = "Ungueltige Bittiefe"; return false; }
        rate_ = rate; bits_ = bits; flac_ = useFlac; block_ = 2 * (bits / 8);
        if (!openFile(pathUtf8, err)) return false;
        if (ring_.empty()) ring_.assign(16u << 20, 0);   // 16 MiB: > 25 s bei 24 Bit/48 kHz
        wr_ = 0; rd_ = 0; frames_ = 0; bytesTotal_ = 0; overruns_ = 0; failed_ = false; stopFlag_ = false;
        minFr_ = 0; maxFr_ = 0; frameNo_ = 0;
        { std::lock_guard<std::mutex> lk(mu_); lastErr_.clear(); cur_ = pathUtf8; }
        rng_ = 0x2545F491u;
        thr_ = std::thread([this] { loop(); });
        active_.store(true, std::memory_order_release);
        return true;
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
    uint64_t frames() const { return frames_.load(); }           // aufgenommene Frames
    uint64_t bytes() const { return bytesTotal_.load(); }        // geschriebene Bytes (Nutzdaten bzw. FLAC-Daten)
    uint32_t overruns() const { return overruns_.load(); }       // verworfene Bloecke (Platte zu langsam)
    int rate() const { return rate_; }
    std::string file() { std::lock_guard<std::mutex> lk(mu_); return cur_; }
    std::string lastError() { std::lock_guard<std::mutex> lk(mu_); return lastErr_; }

    // Audio-Thread: interleaved Stereo-Float. Nie blockierend.
    void push(const float* lr, size_t frames) {
        if (!active_.load(std::memory_order_acquire) || frames == 0) return;
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
        for (size_t i = 0; i < frames * 2; i++) {
            float x = lr[i]; if (!(x == x)) x = 0.0f;   // NaN -> 0
            if (bits_ == 32) { std::memcpy(tmp + t, &x, 4); t += 4; }
            else {
                x = std::max(-1.0f, std::min(1.0f, x));
                if (bits_ == 24) {
                    int32_t v = (int32_t)std::lrintf(x * 8388607.0f);
                    tmp[t++] = (uint8_t)v; tmp[t++] = (uint8_t)(v >> 8); tmp[t++] = (uint8_t)(v >> 16);
                } else {
                    float d = nextU() + nextU() - 1.0f;   // TPDF-Dither, +-1 LSB
                    int32_t v = (int32_t)std::floor(x * 32767.0f + d + 0.5f);
                    v = std::max(-32768, std::min(32767, v));
                    tmp[t++] = (uint8_t)v; tmp[t++] = (uint8_t)(v >> 8);
                }
            }
            if (t > sizeof(tmp) - 8) { put(tmp, t); t = 0; }
        }
        if (t) put(tmp, t);
        wr_.store(w + need, std::memory_order_release);
        cv_.notify_one();
    }

private:
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

    void writeWavHeader(std::FILE* f, uint64_t dataBytes) {
        const bool ext = bits_ != 16, flt = bits_ == 32;
        const uint32_t fmtLen = ext ? 40 : 16;
        hdr_ = 12 + 36 + 8 + fmtLen + (flt ? 12 : 0) + 8;   // 36 = JUNK-Platzhalter (wird bei >4 GiB zu "ds64")
        std::fwrite("RIFF", 1, 4, f); W32(f, (uint32_t)(hdr_ - 8 + dataBytes)); std::fwrite("WAVE", 1, 4, f);
        std::fwrite("JUNK", 1, 4, f); W32(f, 28); { uint8_t z[28] = {0}; std::fwrite(z, 1, 28, f); }
        std::fwrite("fmt ", 1, 4, f); W32(f, fmtLen);
        W16(f, ext ? 0xFFFE : 1); W16(f, 2); W32(f, (uint32_t)rate_); W32(f, (uint32_t)(rate_ * block_)); W16(f, (uint16_t)block_); W16(f, (uint16_t)bits_);
        if (ext) {
            W16(f, 22); W16(f, (uint16_t)bits_); W32(f, 3);   // cbSize, gueltige Bits, Kanalmaske L+R
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
            uint64_t v = ((uint64_t)rate_ << 44) | ((uint64_t)1 << 41) | ((uint64_t)(bits_ - 1) << 36) | (frames_.load() & 0xFFFFFFFFFull);
            for (int i = 0; i < 8; i++) si[6 + i] = (uint8_t)(v >> (56 - 8 * i));
            std::fwrite(si, 1, 14, f_);
        } else {
            const uint64_t frames = data_ / (uint64_t)block_;
            if (data_ <= Rf64Threshold()) {   // klassisches RIFF
                Seek(f_, 4, SEEK_SET); W32(f_, (uint32_t)(hdr_ - 8 + data_));
                if (bits_ == 32) { Seek(f_, hdr_ - 8 - 4, SEEK_SET); W32(f_, (uint32_t)frames); }
                Seek(f_, hdr_ - 4, SEEK_SET); W32(f_, (uint32_t)data_);
            } else {                          // RF64: "RF64" + ds64 (aus dem JUNK-Platzhalter), 32-Bit-Felder = 0xFFFFFFFF
                Seek(f_, 0, SEEK_SET); std::fwrite("RF64", 1, 4, f_); W32(f_, 0xFFFFFFFFu);
                Seek(f_, 12, SEEK_SET); std::fwrite("ds64", 1, 4, f_); W32(f_, 28);
                W64(f_, hdr_ - 8 + data_); W64(f_, data_); W64(f_, frames); W32(f_, 0);
                if (bits_ == 32) { Seek(f_, hdr_ - 8 - 4, SEEK_SET); W32(f_, 0xFFFFFFFFu); }
                Seek(f_, hdr_ - 4, SEEK_SET); W32(f_, 0xFFFFFFFFu);
            }
        }
        std::fflush(f_);
        Seek(f_, 0, SEEK_END);
    }
    bool openFile(const std::string& path, std::string& err) {
        f_ = Open(path);
        if (!f_) { err = "Datei kann nicht angelegt werden: " + path; return false; }
        data_ = 0;
        if (flac_) { std::vector<uint8_t> h = flac::StreamHeader(rate_, bits_, FLAC_BLOCK); std::fwrite(h.data(), 1, h.size(), f_); }
        else writeWavHeader(f_, 0);
        return true;
    }
    void closeFile() {
        if (!f_) return;
        fixHeader(); std::fclose(f_); f_ = nullptr;
    }
    void fail(const std::string& m) {
        { std::lock_guard<std::mutex> lk(mu_); lastErr_ = m; }
        failed_ = true; active_.store(false, std::memory_order_release);
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
        data_ += sz; bytesTotal_ += sz; frames_ += N;
        return true;
    }
    void loop() {
        std::vector<uint8_t> chunk(256 * 1024);
        auto lastFix = std::chrono::steady_clock::now();
        for (;;) {
            const uint64_t r = rd_.load(std::memory_order_relaxed), w = wr_.load(std::memory_order_acquire);
            const size_t avail = (size_t)(w - r);
            bool stopping; { std::lock_guard<std::mutex> lk(mu_); stopping = stopFlag_; }
            if (flac_) {
                const size_t fb = (size_t)FLAC_BLOCK * (size_t)block_;
                if (avail >= fb || (stopping && avail > 0)) {
                    size_t nb = std::min(avail, fb); nb -= nb % (size_t)block_;
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
                    readRing(r, n, chunk.data());
                    if (std::fwrite(chunk.data(), 1, n, f_) != n) { fail("Schreibfehler (Datentraeger voll?)"); break; }
                    data_ += n; bytesTotal_ += n; frames_ += n / (size_t)block_;
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
    std::string cur_, lastErr_;
    int rate_ = 48000, bits_ = 24, block_ = 6;
    bool flac_ = false;
    uint32_t hdr_ = 44, rng_ = 1, minFr_ = 0, maxFr_ = 0;
    uint64_t data_ = 0, frameNo_ = 0;
    std::vector<uint8_t> ring_, enc_;
    std::vector<int32_t> fl_, fr_;
    std::atomic<uint64_t> wr_{0}, rd_{0}, frames_{0}, bytesTotal_{0};
    std::atomic<uint32_t> overruns_{0};
    std::atomic<bool> active_{false}, failed_{false};
    bool stopFlag_ = false;
    std::mutex mu_;
    std::condition_variable cv_;
    std::thread thr_;
};

}  // namespace wavrec
