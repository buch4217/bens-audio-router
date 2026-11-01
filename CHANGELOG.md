# Changelog – Bens Audio Router

## 5.0 (2026) – Studio: Live-Modus, HiRes-Aufnahme, Auto-Pegel, virtueller ASIO-Treiber
- **Live-Modus** (Studio → Live): kleinste Latenz für Eingänge → Ausgänge, z. B. für DJ-Mixer-Fader. Geräte werden mit **IAudioClient3-Niedriglatenz-Shared-Modus** (Windows 10+) bzw. im **Exklusiv-Modus** geöffnet, die Audio-Schleife läuft im **1-ms-Takt** (hochauflösender Timer), der Router-Puffer ist **adaptiv** („so klein wie stabil“: wächst bei Aussetzern, schrumpft bei Ruhe, 2–40 ms, auch fest einstellbar). Lokale Ausgänge bekommen im Live-Modus **keinen Ausgleichs-Delay** des Autopiloten (je Ausgang abschaltbar). Anzeige: Modus, Periode und grobe Eingang→Ausgang-Latenz je Ausgang
- **Pro-Audio je Gerät:** **Exklusiv-Modus** (Windows-Mixer umgehen, bit-genau; automatische Format-Wahl 32/24/16 Bit bei 44,1 … 384 kHz mit sauberem Rückfall in den normalen Modus), wählbare Exklusiv-Samplerate, **Kanalpaar-Wahl** bei Mehrkanal-Eingängen (1/2 … 15/16)
- **Auto-Pegel für die Aufnahme-Box:** Lautheits-Leveler nach **ITU-R BS.1770** (Momentary 400 ms + Short-term 3 s, Ziel −30 … −10 LUFS, max. Anhebung bis 36 dB, Gate gegen Aufpumpen von Stille) und **Look-ahead-True-Peak-Limiter** (Decke −12 … 0 dBTP, ≈ 2 ms Vorlauf). Modi: aus · nur Limiter (Schutz) · Pegeln + Limiter. Messwerte (LUFS, True Peak, Pegel, Limiter, Clips) laufend in App und Web. Neue Boxen starten mit „Pegeln + Limiter“, alte Layouts bleiben unverändert (aus)
- **HiRes-Aufnahme:** Samplerate bis **384 kHz**, **WAV 32 Bit Ganzzahl**, **bit-genaue Wandlung** (Faktor 2²³/2³¹: 24-Bit-Quellen kommen ohne Skalierungsfehler wieder heraus), Dither wählbar (auto/aus/TPDF), **Mehrspur** (jedes Kabel = Stereo-Paar, bis **16 Kanäle** in einer WAV/RF64 mit Kanalmaske), **Bit-perfekt-Modus** (eine Quelle, Quelltakt, kein Resampling/Gain/Limiter), **Vorlauf (Pre-Record)** bis 30 s, **Dateiteilung** nach Minuten (lückenlos, auch FLAC), **BWF-Zeitstempel** (`bext`) für die Platzierung in der DAW, Ringpuffer wächst mit Datenrate (mind. 8 s)
- **Virtuelle Eingänge/Ausgänge und eigener ASIO-Treiber „Bens Virtual Bus“:** 16 Kanäle = **8 Stereo-Eingänge aus der DAW** und **8 Stereo-Ausgänge zur DAW**, **Surround-Zuordnung** (8 × Stereo, 5.1, 7.1, 5.1 × 2, 7.1 × 2, Quad × 4, 7.1.4 mit SMPTE-Kanalnamen) und **Stereo-Downmix** (BS.775) je Surround-Gruppe. Der Treiber steckt **in der EXE**, installiert sich per Taste (einmalige Administrator-Bestätigung) und ist reiner User-Mode-COM-Server (keine Kernel-Komponente, keine Signatur nötig). Samplerate 44,1 … **384 kHz** (die DAW wählt), 32 Bit Float. Eigene Schnittstellen-Deklaration, **kein Steinberg-SDK-Code**
- **Alles auch im Browser:** neuer Reiter **Studio** (Live, Brücke, Surround-Zuordnung, Treiber, Aufnahme-Boxen), erweiterte Einstellungen je Node (Aufnahme-Box mit HiRes/Auto-Pegel/Messwerten, Pro-Audio je Gerät, virtuelle Knoten). Desktop: Toolbar-Taste **Studio…** und Taste im Inspector der Aufnahme-Box. Beide benutzen dieselben Befehle, alle Regeln gelten im Router
- Einstellungen werden im Layout (`.bar`) mitgespeichert (neue `G`- und `P`-Zeilen; ältere Versionen ignorieren sie)
- **Hinweis:** Live-Modus, Exklusiv-Modus, Studio-Fenster und der Brücken-Teil der Engine wurden bisher nur gebaut und gegen Simulationen getestet (Wine, Referenz-Server), noch **nicht auf echter Windows-Hardware**. Bitte mit eigenen Geräten prüfen; Rückmeldungen willkommen. Der Treiber erscheint nur in ASIO-fähigen 64-Bit-DAWs, nicht als Windows-Soundgerät
- Neue Tests: Auto-Pegel (BS.1770-Eichung, True-Peak-Limiter, Leveler, Gate), HiRes-Rekorder (bit-genau, 16 Kanäle, Teilung, Vorlauf), Brücken-Protokoll (4 Mio. Frames parallel), adaptive Latenz, **ASIO-Treiber unter Wine** (COM, Echtzeit-Streaming in beide Richtungen bit-genau, Registrierung, Selbstinstallation der EXE), Web (10 neue Tests, insgesamt 59)

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
