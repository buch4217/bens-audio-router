// Test des Rekorders: WAV 16/24/32 Bit, FLAC 16/24 Bit, eine Datei auch bei RF64, Header waehrend der Aufnahme gueltig.
// Aufruf: ./test_wavrec <ausgabeordner>   Pruefung der Dateien: python3 verify_wavrec.py <ausgabeordner>
#include "wavrec.h"
#include <cstdlib>
#include <thread>
using namespace std::chrono_literals;

static void feed(wavrec::Recorder& r, double sec, int rate, uint64_t& t0) {
    std::vector<float> blk(480 * 2);
    const int blocks = (int)std::lround(sec * rate / 480.0);
    for (int bi = 0; bi < blocks; bi++) {
        for (int i = 0; i < 480; i++, t0++) {
            double t = (double)t0 / rate;
            blk[2 * i] = (float)(0.5 * std::sin(2 * M_PI * 1000.0 * t));
            blk[2 * i + 1] = (float)(0.25 * std::sin(2 * M_PI * 440.0 * t));
        }
        r.push(blk.data(), 480);
    }
}

int main(int argc, char** argv) {
    std::string d = argc > 1 ? argv[1] : "/tmp";
    std::string err;
    for (int bits : {16, 24, 32}) {
        wavrec::Recorder r; uint64_t t0 = 0;
        if (!r.start(d + "/rec_" + std::to_string(bits) + ".wav", 48000, bits, err)) { std::printf("FEHLER: %s\n", err.c_str()); return 1; }
        feed(r, 3.0, 48000, t0); r.stop();
        std::printf("bits=%d frames=%llu bytes=%llu overruns=%u\n", bits, (unsigned long long)r.frames(), (unsigned long long)r.bytes(), r.overruns());
    }
    for (int bits : {16, 24}) {   // FLAC: letzter Frame kuerzer als 4096 (144000 = 35 * 4096 + 640)
        wavrec::Recorder r; uint64_t t0 = 0;
        if (!r.start(d + "/flac_" + std::to_string(bits) + ".flac", 48000, bits, true, err)) { std::printf("FEHLER: %s\n", err.c_str()); return 1; }
        feed(r, 3.0, 48000, t0); r.stop();
        std::printf("flac bits=%d: frames=%llu bytes=%llu overruns=%u\n", bits, (unsigned long long)r.frames(), (unsigned long long)r.bytes(), r.overruns());
    }
    for (int bits : {24, 32}) {   // RF64: Schwelle klein setzen -> dieselbe Umstellung wie bei >4 GiB, aber testbar
        wavrec::Rf64Threshold() = 1u << 20;
        wavrec::Recorder r; uint64_t t0 = 0;
        r.start(d + "/rf64_" + std::to_string(bits) + ".wav", 48000, bits, err); feed(r, 10.0, 48000, t0); r.stop();
        std::printf("rf64 bits=%d: frames=%llu bytes=%llu overruns=%u\n", bits, (unsigned long long)r.frames(), (unsigned long long)r.bytes(), r.overruns());
        wavrec::Rf64Threshold() = 0xFFFFF000ull;
    }
    {   // 96 kHz FLAC (Rate nicht im Frame-Header codierbar -> kommt aus STREAMINFO)
        wavrec::Recorder r; uint64_t t0 = 0;
        r.start(d + "/flac_96k.flac", 96000, 24, true, err); feed(r, 2.0, 96000, t0); r.stop();
    }
    {   // Header bleibt waehrend der Aufnahme gueltig (Absturz-Simulation: Datei kopieren, ohne zu stoppen)
        wavrec::HeaderInterval() = 0.1;
        wavrec::Recorder r; uint64_t t0 = 0;
        r.start(d + "/crash_src.wav", 48000, 24, err); feed(r, 1.0, 48000, t0);
        std::this_thread::sleep_for(500ms);
        std::FILE* a = std::fopen((d + "/crash_src.wav").c_str(), "rb"); std::FILE* b = std::fopen((d + "/crash_copy.wav").c_str(), "wb");
        char buf[65536]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, a)) > 0) std::fwrite(buf, 1, n, b);
        std::fclose(a); std::fclose(b); r.stop();
        std::printf("crash: Kopie waehrend der Aufnahme erstellt, Gesamtframes %llu\n", (unsigned long long)r.frames());
    }
    {   // dasselbe fuer FLAC
        wavrec::Recorder r; uint64_t t0 = 0;
        r.start(d + "/crashf_src.flac", 48000, 24, true, err); feed(r, 1.0, 48000, t0);
        std::this_thread::sleep_for(500ms);
        std::FILE* a = std::fopen((d + "/crashf_src.flac").c_str(), "rb"); std::FILE* b = std::fopen((d + "/crashf_copy.flac").c_str(), "wb");
        char buf[65536]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, a)) > 0) std::fwrite(buf, 1, n, b);
        std::fclose(a); std::fclose(b); r.stop();
    }
    return 0;
}
