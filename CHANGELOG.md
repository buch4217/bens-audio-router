# Changelog – Bens Audio Router

## 4.2 (2026)
- **Neu: Aufnahme-Box.** Neue Senke, die das gemischte Signal direkt aufzeichnet. Aus der Toolbar („● Aufnahme-Box“) per **Drag & Drop aufs Feld ziehen** (Klick oder Menü „+ Ausgang“ geht ebenfalls), Quellen verbinden, **● REC** am Node oder im Inspector
- Formate: **WAV** 24 Bit (Standard), 16 Bit (mit TPDF-Dither), 32 Bit Float oder **FLAC** 24/16 Bit (verlustfrei, etwa halb so groß); 44,1 / 48 / 88,2 / 96 kHz; Zielordner wählbar (Standard `Musik\BensAudioRouter`), Dateiname `Aufnahme_Datum_Uhrzeit.wav/.flac`
- **Nach „Stopp“ liegt immer genau eine fertige Datei vor** (keine Teilung). WAV schaltet ab 4 GB automatisch auf RF64 um, FLAC kennt keine Grenze
- Eigener Schreib-Thread mit 16-MiB-Ringpuffer: Festplatten-Hänger stören das Audio nicht; WAV-/FLAC-Header werden laufend aktualisiert, die Datei bleibt auch nach Absturz oder Stromausfall lesbar
- **Neu: Summen-Modul** („∑ Summen-Modul“ aus der Toolbar aufs Feld ziehen). Eingangs-Port links, Ausgangs-Port rechts: Mono-Quellen lassen sich als **Links/Rechts** zuordnen. Je Kabel: Kanal der Quelle (Stereo, nur Links, nur Rechts, Mono-Summe) → Ziel (Stereo, Links, Rechts, Mitte). Standard: erste Mono-Quelle links, zweite rechts. Optional Mono-Summe am Ausgang
- Das Summen-Modul erzeugt **keine zusätzliche Latenz** (wird beim Mischen aufgelöst); Autopilot und AirPlay-Latenzmeldung rechnen durch das Modul hindurch
- Autopilot und Einmessung ignorieren die Aufnahme-Box; Gain, Mute und Pegelanzeige wirken wie bei jedem Ausgang
- **AirPlay-Empfänger mit Apple-Geräten getestet** (nativ in der AirPlay-Auswahl)
- Neue Tests: `test_wavrec` + `verify_wavrec.py` (WAV/FLAC, Bit-Genauigkeit, RF64, Header-Sicherheit, FLAC bit-identisch zu WAV, `flac -t`), `test_route` (Kanal-Matrix)

## 4.1 (2026)
- **Neu:** AirPlay-Empfänger (RAOP / AirPlay 1): iPhone, iPad und Mac erkennen den Router per Bonjour und spielen als Quelle in das Routing (ALAC 16 Bit / 44,1 kHz)
- Name, Port, Empfangspuffer, Latenz (automatisch aus dem Autopilot oder manuell) und iPhone-Lautstärke einstellbar
- Paketverlust-Behandlung mit automatischer Wiederholungsanforderung; Statusanzeige inkl. mDNS-Prüfung
- Eigene Krypto (SHA-1, AES, RSA-2048) ohne Fremdbibliothek, gegen Python/OpenSSL verifiziert
- Nicht enthalten: AirPlay 2 und AirPlay-Sender (siehe Anleitung, Kapitel 14)

## 4.0 (2026)
- **Neu:** Endloses, zoombares Dashboard (Mausrad-Zoom, Verschieben, „alles anzeigen“), Ansicht wird gespeichert
- **Neu:** Lautstärkeregler pro Sonos-Zone direkt im Dashboard (Haupt-Zone und Gruppenzonen, Ist-Werte per UPnP)
- **Neu:** Branding: App-Icon, Versionsinfo, Info-Dialog, Copyright, Publisher „Buccloud Software (DE)“
- **Neu:** Dokumentation (README, Anleitung als Markdown und HTML), MIT-Lizenz
- Sonos-Gruppen (Koordinator + weitere Zonen, Synchronisation durch Sonos), Pegel zeitversetzt je Zone
- Fix: HTTP-Chunked-Antworten des Sonos werden dekodiert (Namen, Koordinator-ID, Gruppenbeitritt)
- Assistent „Virtuelles Gerät…“ (VB-Audio HIFI-CABLE / VB-CABLE), Standardgerät umschalten und beim Beenden zurücksetzen

## 3.x
- FLAC-24-Bit-Stream (verlustfrei) zum Sonos, automatischer Fallback auf WAV 16 Bit
- Akustische Latenz-Einmessung (Messsweep, FFT-Kreuzkorrelation) und Autopilot
- Sonos-Positionsmessung (Drift-Tracking), Layouts, Dark-Mode

## 2.x / 1.x
- Node-Router mit WASAPI-Loopback/Eingang/Testgenerator, VU-Metern, Delay, Sinc-Resampler, TPDF-Dither
