# Changelog – Bens Audio Router

## 4.5 (2026) – Hintergrund-Betrieb und voller Funktionsumfang im Browser
- **Hintergrund-Betrieb:** Tray-Symbol (Links = Fenster, Rechts = Menü), **Autostart mit Windows** (Registry „Run“, nur Benutzer, ohne Administrator), **Schließen = im Tray weiterlaufen**, versteckter Start (`--background`), **Engine automatisch starten**, zweite Programminstanz holt das laufende Fenster nach vorn, Toolbar-Taste „Hintergrund…“; bewusst **kein Windows-Dienst** (Sitzung 0 hat keinen Zugriff auf Audio-Sitzung, Profil und Zertifikat)
- **Web: voller Funktionsumfang wie die Desktop-App** – neuer System-Reiter (Engine Start/Stopp, Geräte neu einlesen, Sonos-Suche und Stream anlegen, Puffer, Virtuelles Gerät, Einmessen mit PC-Mikrofon, Synchronisations-Übersicht), vollständige Einstellungen je Node (Sonos-Stream inkl. Gruppe, AirPlay, Aufnahme-Box, Test-Signal, Nachgelagerte Latenz), Szenen löschen, Nodes im Graph verschieben, Statusmeldungen der App am Handy, Eingabefelder ohne iOS-Zoom
- Alle neuen Befehle werden im Router **wie die Desktop-Regler geprüft** (Wertebereiche, Nebenwirkungen wie „Node neu verbinden“); neue Tests: Protokoll (Wertebereiche, IP-Format, Ordner-Prüfung), Oberfläche (jedes Menü sendet den richtigen Befehl)

## 4.4 (2026) – Feinjustage, Einmessen per Handy, Gain, Toolbar
- **Gain nur bis 0 dB** (App und Web): kein Pluspegel mehr; **Doppelklick** auf den Gain-Regler (Web: Doppeltippen oder Knopf „0“) setzt auf 0 dB zurück; alte Layouts werden beim Laden auf ≤ 0 dB begrenzt
- **Web: Delay feinjustieren** – Trim (±1 / ±0,1 ms) an Ausgängen, bei ausgeschaltetem Autopilot zusätzlich Delay je Node
- **Web: Einmessen mit dem Handy-Mikrofon** – neuer Reiter „Messen“: das Handy streamt sein Mikrofon als Quelle „Handy-Mikrofon“ (binäre WebSocket-Frames mit Zeitstempel-Abgleich), der vorhandene Einmess-Ablauf misst jeden Ausgang und der Autopilot gleicht für den Hörplatz aus
- **Web: Eingänge und Ausgänge hinzufügen/entfernen** (Geräte-Listen aus dem Router, Test-Signale, AirPlay, Aufnahme-Box, Summen-Modul)
- **Web: Fader-Fix** – die Oberfläche wird während des Ziehens nicht mehr neu aufgebaut (Fader konnten sonst abbrechen)
- **iPhone/Browser-Symbole:** Apple-Touch-Icon (180 px), Favicon, Meta-Tags für „Zum Home-Bildschirm“
- **Web: Oberfläche neu gestaltet** – kompakte Karten mit einheitlichen Symbolen, schönere Fader mit Füllung, Mute als Symbol-Knopf, einklappbarer „Timing“-Bereich (Delay/Trim), „+“-Knopf, Raster für Tablet/Querformat mit Seitennavigation, Scroll-Position bleibt beim Aktualisieren erhalten, Vollbild ohne leeren Streifen unten (iPhone)
- **Web: Kopfbereich solide und scharf** (kein Farbverlauf/Leuchten), Tab-Leiste ohne abgeschnittene Bereiche, „+“-Knopf im Kopf statt schwebend; Info-Reiter mit „Anzeige-Diagnose“
- **Web (iPhone-App): Darstellung folgt dem wirklich sichtbaren Bereich** (iOS meldet in Home-Bildschirm-Apps teils eine zu kleine Höhe), Doppeltipp-Zoom abgeschaltet, **Tab-Leiste seitlich wischbar** auf kleinen Bildschirmen, „Neu messen“ in der Anzeige-Diagnose
- **iPhone-Symbol direkt in die Seite eingebettet** (Data-URI), damit „Zum Home-Bildschirm“ kein zweites Mal vom Router laden muss (das scheiterte ohne installierte CA)
- **Toolbar:** bei schmalem Fenster wandern nicht passende Schaltflächen ins Menü „⋯“ (nichts wird mehr abgeschnitten)

## 4.3 (2026) – Web-Interface
- **Neu: Web-Interface** – den Router **am Handy bedienen**. Toolbar „Web-Interface…“: einschalten, Port, QR-Code zum Koppeln, Geräteliste, CA-Zertifikat speichern. Standardmäßig **aus**
- **Mobile Web-App** (eine Datei, in der EXE eingebettet): Mixer (Fader, Mute, Live-Pegel, Sonos-Zonen, REC mit Timer), **Matrix-Patchbay** (Tippen = verbinden/lösen, lange drücken = Details und Kanalzuordnung am Summen-Modul), Graph (Port antippen → Ziel antippen, Pinch-Zoom), Szenen (Layouts laden/speichern), Autopilot, Info. Als „Zum Home-Bildschirm“-App installierbar
- **Sicherheit:** nur **HTTPS** (TLS 1.2+, Windows SChannel, **eigene lokale CA** + Server-Zertifikat mit Hostname und IP, kein HTTP-Port), nur Verbindungen aus dem **privaten Netz**, Kopplung per **8-stelligem Einmal-Code** (120 s) mit Sperre nach 5 Fehlversuchen, Sitzungs-Cookie `Secure; HttpOnly; SameSite=Strict`, Token nur als Hash gespeichert, Origin-/CSRF-Prüfung, CSP mit Hashes, Befehle werden im Router **vollständig serverseitig geprüft**
- Router-Kern portabel und getestet: `webcore.h` (HTTP, WebSocket, Sitzungen), `qr.h` (QR-Code-Encoder), `web/mock-server.js` (Referenz-Server); 17 + 14 + 9 Web-Tests, QR-Test mit OpenCV-Decoder
- Kabel haben stabile Nummern (für die Fernbedienung)

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
