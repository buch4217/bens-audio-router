# Drittanbieter-Hinweise – Bens Audio Router 5.0

**Publisher:** Buccloud Software (DE) · © Ben Buchholz · www.buccloud.de · 2026

| Komponente | Herkunft / Lizenz | Verwendung |
|---|---|---|
| `alac.c` / `alac.h` | ALAC-Decoder, © 2005 David Hammerton, MIT-Lizenz (aus dem Projekt *shairport-sync*, MIT). Einzige Anpassung: `stdint.h`-Include für MinGW. | Dekodierung von Apple Lossless beim AirPlay-Empfänger |
| RAOP-RSA-Schlüssel (`ap_key.h`) | Öffentlich dokumentierter Schlüssel des AirPlay-1-Protokolls (u. a. in *shairport-sync*, MIT, enthalten) | Apple-Challenge und AES-Schlüsselübergabe bei AirPlay 1 (RAOP) |
| MinGW-w64-Laufzeit | statisch gelinkt (GCC Runtime Library Exception) | Build-Umgebung |

Alle weiteren Teile (DSP, Sinc-Resampler, FLAC-Encoder, Einmessung, Sonos/UPnP, UI, AirPlay-Protokoll, SHA-1/AES/RSA) sind eigener Code unter der MIT-Lizenz.

Marken: Sonos, AirPlay, Apple, Bonjour, Windows und VB-Audio sind Marken ihrer jeweiligen Inhaber. Dieses Projekt steht in keiner Verbindung zu ihnen.
Treiber von Drittanbietern (z. B. VB-Audio HIFI-CABLE) sind nicht enthalten.

## Neu in 5.0

- **ASIO-Treiber „Bens Virtual Bus“** (`driver/`): eigene, saubere Deklaration der öffentlichen ASIO-Treiberschnittstelle (`asio_iface.h`), **kein Code und keine Header des Steinberg-ASIO-SDKs**. „ASIO“ ist eine Marke der Steinberg Media Technologies GmbH; dieses Projekt ist nicht von Steinberg. Das Projekt bleibt MIT-lizenziert.
- **Auto-Pegel** (`dyn.h`): K-Bewertungsfilter und Lautheitsformel nach der öffentlichen Empfehlung ITU-R BS.1770 (eigene Implementierung); True-Peak-Erkennung mit eigenem Kaiser-Sinc-Interpolator.
- **Tests:** Wine (LGPL) wird nur zum Ausführen der Windows-Testprogramme unter Linux benutzt und ist nicht Teil des Programms. `ffmpeg`/`flac`/`numpy` dienen nur als Referenz in den Tests.
- Keine neuen Laufzeit-Bibliotheken: die EXE bleibt eine einzelne Datei ohne Fremdbibliotheken.
