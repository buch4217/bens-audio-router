// HiRes-Tests des Rekorders: 16 Kanaele, 32-Bit-Ganzzahl, bit-genaue Wandlung, Teilung, Vorlauf (Pre-Record), BWF-Zeitstempel, Fehlerfaelle.
// Aufruf: ./test_wavrec_hires <ordner>   Pruefung: python3 verify_wavrec_hires.py <ordner>
#include "wavrec.h"
#include <cstdlib>
using namespace wavrec;
// Testmuster: Ganzzahl-Wert je (Frame, Kanal), garantiert 24-Bit-genau darstellbar
static int32_t Pat(uint64_t k, int c) { return (int32_t)(((k * 2654435761ull + (uint64_t)c * 40503ull * 977ull) >> 3) % 16777216ull) - 8388608; }
static void Feed(Recorder& r, uint64_t from, uint64_t frames, int ch, bool monitor = false) {
    std::vector<float> blk(480 * (size_t)ch);
    for (uint64_t done = 0; done < frames;) { size_t n = (size_t)std::min<uint64_t>(480, frames - done);
        for (size_t i = 0; i < n; i++) for (int c = 0; c < ch; c++) blk[i * (size_t)ch + (size_t)c] = (float)Pat(from + done + i, c) / 8388608.0f;
        if (monitor) r.monitor(blk.data(), n); else r.push(blk.data(), n); done += n; }
}
int main(int argc, char** argv) {
    std::string d = argc > 1 ? argv[1] : "/tmp", err; int fails = 0;
    auto expectFail = [&](Options o, const char* what) { Recorder r; std::string e; bool ok = r.start(o, e); if (ok) { printf("FEHLER: %s wurde akzeptiert\n", what); fails++; r.stop(); } else printf("ok: %s abgelehnt (%s)\n", what, e.c_str()); };
    { Recorder r; Options o; o.path = d + "/hr_16ch_96k_24.wav"; o.rate = 96000; o.bits = 24; o.channels = 16; o.description = "16-Kanal-Test"; o.timeRefSamples = 123456789ull;
      if (!r.start(o, err)) { printf("FEHLER %s\n", err.c_str()); return 1; } Feed(r, 0, 96000 * 5, 16); r.stop(); printf("16ch/96k/24: frames=%llu bytes=%llu overruns=%u\n", (unsigned long long)r.frames(), (unsigned long long)r.bytes(), r.overruns()); fails += r.overruns() != 0; }
    { Recorder r; Options o; o.path = d + "/hr_int32_192k.wav"; o.rate = 192000; o.bits = 32; o.int32 = true; o.channels = 2;
      r.start(o, err); std::vector<float> b(480 * 2); for (uint64_t k = 0; k < 192000ull * 3; k += 480) { for (int i = 0; i < 480; i++) for (int c = 0; c < 2; c++) b[2 * i + c] = (float)Pat(k + i, c) / 8388608.0f; r.push(b.data(), 480); } r.stop(); printf("int32/192k: frames=%llu\n", (unsigned long long)r.frames()); }
    { Recorder r; Options o; o.path = d + "/hr_8ch_f32_96k.wav"; o.rate = 96000; o.bits = 32; o.channels = 8; r.start(o, err); Feed(r, 0, 96000 * 2, 8); r.stop(); }
    { Recorder r; Options o; o.path = d + "/hr_16bit_nodither.wav"; o.rate = 44100; o.bits = 16; o.dither = 0; r.start(o, err);
      std::vector<float> b(2 * 441); for (int blk = 0; blk < 100; blk++) { for (int i = 0; i < 441; i++) for (int c = 0; c < 2; c++) b[2 * i + c] = (float)(((blk * 441 + i) * 37 + c * 1001) % 65536 - 32768) / 32768.0f; r.push(b.data(), 441); } r.stop(); }
    { Recorder r; Options o; o.path = d + "/hr_split.wav"; o.rate = 48000; o.bits = 24; o.splitSeconds = 1.0; o.timeRefSamples = 1000;
      r.start(o, err); Feed(r, 0, 48000 * 3 + 24000, 2); r.stop(); printf("Teilung: %u Dateien, frames=%llu\n", r.parts(), (unsigned long long)r.frames()); fails += r.parts() != 4; }
    { Recorder r; Options o; o.path = d + "/hr_split_bytes.wav"; o.rate = 48000; o.bits = 24; o.channels = 2; o.splitBytes = 300000;
      r.start(o, err); Feed(r, 0, 48000 * 4, 2); r.stop(); printf("Teilung nach Bytes: %u Dateien\n", r.parts()); }
    { Recorder r; Options o; o.path = d + "/hr_split.flac"; o.rate = 48000; o.bits = 24; o.flac = true; o.splitSeconds = 1.0;
      r.start(o, err); Feed(r, 0, 48000 * 2 + 10000, 2); r.stop(); printf("FLAC-Teilung: %u Dateien\n", r.parts()); fails += r.parts() != 3; }
    { Recorder r; r.arm(48000, 2, 2.0); Feed(r, 0, 48000 * 3, 2, true);   // 3 s laufen lassen, nur die letzten 2 s bleiben im Vorlauf
      Options o; o.path = d + "/hr_preroll.wav"; o.rate = 48000; o.bits = 24; r.start(o, err); Feed(r, 48000 * 3, 48000, 2, true); r.stop();
      printf("Vorlauf: frames=%llu (erwartet %d)\n", (unsigned long long)r.frames(), 48000 * 3); fails += r.frames() != 48000ull * 3; }
    { Recorder r; r.arm(48000, 2, 2.0); Feed(r, 0, 48000, 2, true); Options o; o.path = d + "/hr_preroll_mismatch.wav"; o.rate = 48000; o.bits = 24; o.channels = 4; r.start(o, err); Feed(r, 0, 4800, 4); r.stop(); fails += r.frames() != 4800; }   // falsche Kanalzahl: Vorlauf wird nicht verwendet
    Options bad; bad.path = d + "/x.flac"; bad.flac = true; bad.channels = 6; expectFail(bad, "FLAC mit 6 Kanaelen");
    bad = Options(); bad.path = d + "/x.wav"; bad.channels = 17; expectFail(bad, "17 Kanaele");
    bad = Options(); bad.path = d + "/x.wav"; bad.rate = 1000; expectFail(bad, "Rate 1000 Hz");
    bad = Options(); bad.path = d + "/x.wav"; bad.bits = 20; expectFail(bad, "20 Bit");
    printf(fails ? "\n%d FEHLER\n" : "\nC++-Teil bestanden\n", fails); return fails ? 1 : 0;
}
