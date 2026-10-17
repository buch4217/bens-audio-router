# Mitmachen

Danke für dein Interesse an **Bens Audio Router** (Buccloud Software (DE))! Fehlermeldungen, Ideen und Pull Requests sind willkommen.

## Fehler melden
Öffne ein Issue mit der Vorlage „Fehlerbericht“. Hilfreich sind: Windows-Version, was du gemacht hast, die Datei `%APPDATA%\BensAudioRouter\log.txt`
und (wenn möglich) ein Screenshot. **Keine** Passwörter, Tokens oder privaten Netzwerkdaten posten.

## Code beitragen
1. Fork anlegen, Branch erstellen (`feature/mein-thema`).
2. Bauen: `sudo apt install g++-mingw-w64-x86-64` und `./src/build.sh` (siehe README).
3. Tests laufen lassen (siehe `tests/README.md`).
4. Pull Request mit kurzer Beschreibung: *Was* und *Warum*, getestet womit (echte Geräte oder Simulation).

**Stil:** C++17, keine Fremdbibliotheken in der EXE (Windows-Bordmittel), Kommentare auf Deutsch oder Englisch, klare Fehlermeldungen im UI.
**Bitte beachten:** Änderungen an Audio-Kern (`core.h`), FLAC (`flac.h`) oder Einmessung (`calib.h`) mit einem Test belegen.

## Besonders gesucht
- Tests mit **echten Apple-Geräten** (AirPlay-Empfänger) und **echten Sonos-Geräten** unterschiedlicher Modelle
- AirPlay-1-Sender (Router → AirPlay-Lautsprecher)
- Übersetzungen der Oberfläche und Anleitung

Mit deinem Beitrag stimmst du zu, dass er unter der **MIT-Lizenz** des Projekts veröffentlicht wird.
