#!/bin/bash
# Fuehrt alle Tests aus (Linux). Voraussetzungen: g++, g++-mingw-w64-x86-64, python3 mit numpy, node, ffmpeg, flac; fuer die ASIO-Treiber-Tests zusaetzlich wine64.
set -e
cd "$(dirname "$0")"
T=/tmp/bar_tests; mkdir -p $T; FAIL=0
run() { echo "== $1"; shift; if ! "$@"; then echo "!! FEHLGESCHLAGEN: $*"; FAIL=1; fi; }
g++ -O3 -ffast-math -std=c++17 -I../src test_core.cpp -o $T/test_core && run "Resampler (core)" $T/test_core
g++ -O2 -std=c++17 -I../src test_calib.cpp -o $T/test_calib && run "Einmessung" $T/test_calib
g++ -O2 -std=c++17 -I../src test_route.cpp -o $T/test_route && run "Kanal-Matrix" $T/test_route
g++ -O2 -std=c++17 -I../src test_dyn.cpp -o $T/test_dyn && run "Auto-Pegel (Lautheit, True Peak)" $T/test_dyn
g++ -O2 -std=c++17 -pthread -I../src test_vbus.cpp -o $T/test_vbus && run "virtuelle Bruecke (Protokoll)" $T/test_vbus
g++ -O2 -std=c++17 -I../src test_livectl.cpp -o $T/test_livectl && run "adaptive Live-Latenz" $T/test_livectl
g++ -O2 -std=c++17 -pthread -I../src test_wavrec.cpp -o $T/test_wavrec && mkdir -p $T/wt && rm -f $T/wt/* && $T/test_wavrec $T/wt >/dev/null && run "Rekorder (WAV/FLAC/RF64)" python3 verify_wavrec.py $T/wt
g++ -O2 -std=c++17 -pthread -I../src test_wavrec_hires.cpp -o $T/test_wavrec_hires && mkdir -p $T/wh && rm -f $T/wh/* && $T/test_wavrec_hires $T/wh >/dev/null && run "Rekorder HiRes (16 Kanaele, Teilung, Vorlauf)" python3 verify_wavrec_hires.py $T/wh
g++ -O2 -std=c++17 -I../src test_qr.cpp -o /tmp/test_qr && run "QR-Code (OpenCV)" python3 verify_qr.py
g++ -O2 -std=c++17 -pthread -I../src test_webcore.cpp -o $T/test_webcore
run "Web (Logik, Protokoll, Oberflaeche, C++-Serverkern)" bash -c "cd .. && BAR_WEBTEST_BIN=$T/test_webcore node --test --test-force-exit web/test/*.test.js"
if command -v wine64 >/dev/null 2>&1 || [ -x /usr/lib/wine/wine64 ]; then
  W=$(command -v wine64 || echo /usr/lib/wine/wine64); export WINEDEBUG=-all WINEPREFIX=$T/wineprefix
  (cd ../src && ./build.sh >/dev/null 2>&1) || true
  x86_64-w64-mingw32-g++-posix -std=c++17 -O2 -static -static-libgcc -static-libstdc++ asio_host.cpp -o $T/asio_host.exe -lole32 -luuid -ladvapi32 -lwinmm
  run "ASIO-Treiber unter Wine (COM, Streaming bit-genau, Registrierung)" $W $T/asio_host.exe ../driver/bensasio.dll
  run "EXE: Treiber selbst installieren" $W ../src/BensAudioRouter.exe --driver-install
  run "EXE: Treiber wieder entfernen" $W ../src/BensAudioRouter.exe --driver-remove
else echo "(wine64 nicht gefunden - ASIO-Treiber-Tests uebersprungen)"; fi
[ $FAIL = 0 ] && echo "ALLE TESTS BESTANDEN" || { echo "FEHLER"; exit 1; }
