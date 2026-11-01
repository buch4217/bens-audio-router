<p align="center"><img src="docs/banner.png" alt="Bens Audio Router" width="100%"></p>

# Bens Audio Router

![Lizenz](https://img.shields.io/badge/Lizenz-MIT-blue) ![Plattform](https://img.shields.io/badge/Plattform-Windows%2010%2F11%20x64-0078d4) ![Version](https://img.shields.io/badge/Version-5.0-brightgreen) ![Status](https://img.shields.io/badge/Status-Open%20Source-orange)

**Audiophiler Node-Router für Windows – Mitschnitt, lokale Ausgänge und Sonos-Streaming mit automatischem Latenz-Ausgleich.**

Ein Programm von **Buccloud Software (DE)** · © Ben Buchholz · [www.buccloud.de](https://www.buccloud.de) · 2026 · Open Source (MIT)

---

## Was ist das?

Bens Audio Router verbindet Audioquellen und Ausgänge in einem grafischen Dashboard per Drag & Drop. Du ziehst Kabel von
einer Quelle zu mehreren Ausgängen – zum Beispiel vom PC zum lokalen Studio **und** zu einer ganzen **Sonos-Gruppe** – und der
**Autopilot** gleicht die unterschiedlichen Laufzeiten automatisch aus, damit überall zeitgleich gespielt wird.

![Dashboard](docs/screenshots/dashboard.png)

## Highlights

| | |
|---|---|
| **Node-Dashboard** | Endlos, zoombar, Drag & Drop, Mischen von Quellen, VU-Meter (Stereo, Peak-Hold, Clip) |
| **Audiophile Signalkette** | 32-Bit-Float, Kaiser-Sinc-Resampler (~90 dB Sperrdämpfung), TPDF-Dither nur am Ende, Drift-Regelung im ppm-Bereich |
| **Sonos-Streaming** | UPnP + **FLAC 24 Bit (verlustfrei)** oder WAV 16 Bit, Auto-Fallback, Puffer einstellbar |
| **Sonos-Gruppen** | Koordinator + weitere Zonen (Sonos synchronisiert selbst), **Lautstärkeregler pro Zone** im Dashboard |
| **Autopilot & Einmessen** | Latenz wird gemessen (Mikrofon-Messsweep, Sonos-Position) und automatisch ausgeglichen; Feinkorrektur manuell |
| **Virtuelles Gerät** | Assistent für VB-Audio HIFI-CABLE / VB-CABLE: verkabelt alles, schaltet Windows um, stellt beim Beenden zurück |
| **Layouts** | Konfigurationen als `.bar`-Dateien speichern und laden („Party“, „Alltag“ …) |
| **AirPlay-Empfänger** | iPhone/iPad/Mac spielen nativ per AirPlay direkt in den Router (mit Apple-Geräten getestet; AirPlay 1/RAOP, ALAC 16 Bit/44,1 kHz), Name und Latenz einstellbar |
| **Aufnahme-Box** | Zeichnet das gemischte Signal als **WAV** (16/24 Bit, 32 Bit Float) oder **FLAC** (16/24 Bit, verlustfrei) auf, 44,1–96 kHz. Per Drag & Drop aus der Toolbar aufs Feld ziehen, Quellen verbinden, **● REC**. Immer **genau eine Datei** pro Aufnahme (WAV ab 4 GB automatisch RF64), eigener Schreib-Thread, absturzsicherer Header |
| **Summen-Modul** | Für Mono-Eingänge: zwei Mono-Quellen werden als **Links/Rechts** zugeordnet (je Kabel: Kanal der Quelle → Links, Rechts, Mitte oder Stereo). Beim Mischen aufgelöst: **keine zusätzliche Latenz**, der Autopilot rechnet wie mit direkten Kabeln |
| **Hintergrund-Betrieb** | **Tray-Symbol**, **Autostart mit Windows**, „Schließen = im Tray weiterlaufen“, versteckter Start, Engine startet automatisch; eine zweite Programmstart holt das laufende Fenster nach vorn |
| **Web-Interface (Handy)** | Router am **Handy bedienen**: Mixer mit Live-Pegeln, **Matrix-Patchbay** zum Verkabeln per Tippen, Graph mit Pinch-Zoom, Szenen, REC, Autopilot. Nur **HTTPS** (eigene lokale CA), Kopplung per QR-Code und Einmal-Code, nur im privaten Netz, standardmäßig aus |
| **Live-Modus** | Kleinste Latenz für Eingänge → Ausgänge (z. B. DJ-Mixer-Fader): Niedriglatenz-Pfad (IAudioClient3) oder **Exklusiv-Modus**, 1-ms-Takt, **adaptiver Puffer** („so klein wie stabil“, 2–40 ms), kein Ausgleichs-Delay auf lokalen Ausgängen |
| **Auto-Pegel** | Aufnahme nie zu leise, nie übersteuert: **Lautheits-Leveler (BS.1770)** + **True-Peak-Limiter**, Zielpegel, Decke und Reaktion einstellbar, Messwerte live |
| **HiRes-Aufnahme** | bis **384 kHz**, WAV 16/24/32 (Float oder Ganzzahl), FLAC, **bit-genau**, **Mehrspur bis 16 Kanäle**, **Bit-perfekt-Modus**, Vorlauf (Pre-Record), Dateiteilung, BWF-Zeitstempel |
| **Virtueller ASIO-Treiber** | **„Bens Virtual Bus“**: 16 Kanäle = **8 Stereo-Eingänge aus der DAW + 8 Stereo-Ausgänge zur DAW**, Surround-Zuordnung (5.1, 7.1, 7.1.4 …) mit Stereo-Downmix. In der EXE eingebettet, per Taste installiert, reiner User-Mode-Treiber |
| **Modernes UI** | Natives Dark-Mode (auch Titelleiste und Menüs), Auto/Dunkel/Hell |

## Download

Fertige Programme findest du unter **[Releases](../../releases)** (`BensAudioRouter-win64.zip`: EXE, Anleitung, Lizenz). Die EXE ist **nicht code-signiert**; Windows SmartScreen kann deshalb beim ersten Start warnen („Weitere Informationen → Trotzdem ausführen“). Du kannst sie jederzeit selbst aus dem Quelltext bauen (siehe unten).

## Schnellstart

1. **Virtuellen Treiber installieren** (einmalig): [VB-Audio HIFI-CABLE](https://vb-audio.com/Cable/) bzw. VB-CABLE, als Administrator, danach neu starten.
2. `BensAudioRouter.exe` starten (keine Installation nötig).
3. Toolbar → **Virtuelles Gerät… → Assistent**. Er legt Quelle, lokalen Ausgang und Kabel an und stellt Windows um.
4. **+ Stream-Ausgang** → Sonos-Zone wählen → im Inspector **weitere Zonen** hinzufügen.
5. Optional: **+ Quelle → AirPlay-Empfänger**, dann am iPhone im AirPlay-Menü „Bens Audio Router“ wählen.
6. **Start** drücken. Der **Autopilot** verzögert das lokale Studio passend zum Sonos.
7. Optional: Mikrofon als Quelle hinzufügen und **Einmessen…** für die exakte Latenz.

Die vollständige Bedienung steht in der **[Anleitung](docs/Anleitung.md)** (auch als `Anleitung.html`).

## Systemvoraussetzungen

- Windows 10 (1903+) oder Windows 11, 64 Bit
- Für lokales Routing mit eigenem Gerät: virtueller Audiotreiber (z. B. VB-Audio HIFI-CABLE) – **oder** der mitgelieferte ASIO-Treiber „Bens Virtual Bus“ für DAWs (Installation per Taste in „Studio…“, einmalige Administrator-Bestätigung)
- Für den Live-Modus mit minimaler Latenz: Windows 10 (Niedriglatenz-Shared) bzw. ein Audiogerät mit Exklusiv-Modus; die erreichbare Latenz hängt vom Gerätetreiber ab
- Für Sonos: Sonos S2 oder S1 im selben Netzwerk (UPnP); Windows-Firewall: privates Netz freigeben

## Aus dem Quelltext bauen

Cross-Compile unter Linux mit MinGW-w64 (oder unter Windows mit MSYS2):

```bash
sudo apt install g++-mingw-w64-x86-64
cd src && ./build.sh        # baut zuerst den ASIO-Treiber (../driver), bettet ihn ein und erzeugt BensAudioRouter.exe
tests/run_all.sh            # alle Tests (nativ; ASIO-Treiber-Tests laufen mit Wine, falls vorhanden)
```

Aufbau: `BensAudioRouter.cpp` (App), `core.h` (DSP), `flac.h` (FLAC-Encoder), `calib.h` (Einmessung), `stream.h` (Sonos/UPnP/HTTP), `airplay.h` + `ap_crypto.h` (AirPlay-Empfänger), `alac.c` (ALAC-Decoder), `wavrec.h` (WAV/FLAC-Rekorder der Aufnahme-Box), `route.h` (Kanal-Matrix des Summen-Moduls), `webcore.h` + `webwin.h` + `qr.h` + `web/` (Web-Interface: Server-Kern, TLS/CA, QR-Kopplung, mobile Web-App),
`ui.h` (Theme und Controls), `dyn.h` (Auto-Pegel: Lautheit, True-Peak-Limiter), `vbus.h` + `vbus_win.h` (virtuelle Brücke), `livectl.h` (adaptive Live-Latenz), `driver/` (ASIO-Treiber „Bens Virtual Bus“), `tests/` (Kern-Tests), `tools/mock_sonos.py` (Sonos-Simulator) und `tools/raop_test_sender.py` (AirPlay-Testsender).

## Status und Roadmap

- **AirPlay-Empfänger (AirPlay 1 / RAOP)** ist mit Apple-Geräten getestet und arbeitet dort nativ (der Router erscheint direkt in der AirPlay-Auswahl). Zusätzlich mit einem eigenen Testsender geprüft (Challenge, Verschlüsselung, ALAC, Paketverlust mit Resend, mDNS). Rückmeldungen zu weiteren Geräten und iOS-/macOS-Versionen sind willkommen.
- **AirPlay 2** (Empfänger und Sender) ist **nicht** enthalten: HomeKit-Pairing, ChaCha20, PTP, AAC und FairPlay sind sehr umfangreich, die Referenzimplementierung unterstützt Windows nicht, und ohne reale Gegengeräte wäre das Ergebnis nicht überprüfbar.
- Geplant: AirPlay-1-Sender (Router → AirPlay-Lautsprecher); AirPlay 2 erst mit einem Zielgerät zum Testen.
- Sonos Connect (Gen 2, 2015) unterstützt kein AirPlay (Herstellerangabe); AirPlay-fähige Sonos-Geräte lassen sich per UPnP-Gruppe einbinden.

## Mitmachen

Fehler melden, Ideen einbringen, testen – siehe [CONTRIBUTING.md](CONTRIBUTING.md). Besonders hilfreich sind Tests mit echten Apple- und Sonos-Geräten. Sicherheitsrelevantes bitte laut [SECURITY.md](SECURITY.md) melden.

## Lizenz und Hinweise

MIT-Lizenz, siehe [LICENSE](LICENSE). Sonos, AirPlay, Apple, Windows und VB-Audio sind Marken ihrer jeweiligen Inhaber; dieses Projekt steht in keiner Verbindung zu ihnen.

**© Ben Buchholz · [www.buccloud.de](https://www.buccloud.de) · 2026 · Software Publisher: Buccloud Software (DE)**
