# Bens Virtual Bus – ASIO-Treiber (Version 5.0)

16 Kanäle (= **8 Stereo-Paare**) zwischen DAW und **Bens Audio Router**. MIT-Lizenz, © Ben Buchholz · Buccloud Software (DE).

| | |
|---|---|
| Art | reiner **User-Mode-COM-Server** (x64-DLL), keine Kernel-Komponente, keine Treibersignatur nötig |
| Kanäle | 16 Ausgänge der DAW → Router („virtuelle Eingänge“), 16 Eingänge der DAW ← Router („virtuelle Ausgänge“) |
| Format | 32 Bit Float, 44,1 · 48 · 88,2 · 96 · 176,4 · 192 · 352,8 · 384 kHz (die DAW wählt; 48 kHz ist nur der Startwert), Puffer 32 … 2048 Frames (Zweierpotenzen) |
| Transport | gemeinsamer Speicher `Local\BensVBus1`, zwei lock-freie Ringe (siehe `../src/vbus.h`) |
| Takt | der Treiber (Systemuhr/QPC, hochauflösender Timer, MMCSS „Pro Audio“); der Router läuft auf derselben Uhr |
| Kanalnamen | folgen der im Router gewählten Surround-Zuordnung (SMPTE: `5.1 C`, `7.1.4 Ltf` …) |

## Installation

Am einfachsten im Router: **Studio… → Treiber installieren…** (einmalige Windows-Administrator-Bestätigung). Die DLL steckt in `BensAudioRouter.exe` (Ressource 101) und wird nach `%ProgramData%\BensAudioRouter\BensAsioBridge.dll` entpackt und in `HKLM\SOFTWARE\ASIO\Bens Virtual Bus` angemeldet.

Von Hand (Administrator-Eingabeaufforderung): `regsvr32 bensasio.dll` bzw. `regsvr32 /u bensasio.dll`. Die EXE kennt auch `--driver-install` und `--driver-remove` (nur mit Administratorrechten, kein Fenster).

## Bauen

`src/build.sh` baut die DLL zuerst und bettet sie ein. Einzeln:

```bash
x86_64-w64-mingw32-g++-posix -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ bensasio.cpp -o bensasio.dll -lole32 -loleaut32 -luuid -ladvapi32 -lwinmm -lavrt
```

## Test

`tests/asio_host.cpp` verhält sich wie eine DAW (lädt die DLL, legt 16 + 16 Puffer an, läuft 4 s im Echtzeit-Takt) und simuliert gleichzeitig die Router-Seite. Geprüft werden COM/Klassenfabrik, Kanalnamen, Raten, Puffergrößen, Streaming in beide Richtungen **bit-genau** und Registrierung. Linux: `tests/run_all.sh` (nutzt Wine).

## Hinweise

- Die Schnittstelle (`asio_iface.h`) ist eine **eigene Deklaration** der öffentlichen ASIO-Treibermethoden; sie enthält keinen Code des Steinberg-ASIO-SDKs. „ASIO“ ist eine Marke der Steinberg Media Technologies GmbH.
- Der Treiber erscheint nur in **ASIO-fähigen Programmen**, nicht als Windows-Soundgerät (WASAPI). Ein Windows-Soundgerät bräuchte einen signierten Kernel-Treiber (WDK).
- Läuft der Router nicht, liefert der Treiber Stille und die DAW läuft normal weiter.
