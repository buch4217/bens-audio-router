# Changelog – Bens Audio Router

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
