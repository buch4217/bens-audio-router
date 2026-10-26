# Sicherheit

Bens Audio Router öffnet bewusst **lokale Netzwerkdienste**, wenn die entsprechenden Funktionen genutzt werden:

| Funktion | Port | Hinweis |
|---|---|---|
| Sonos-Stream (HTTP) | TCP 8100 (einstellbar) | liefert den Audiostream an den Sonos |
| AirPlay-Empfänger | TCP 5000, UDP 6000–6200, UDP 5353 (mDNS) | nur wenn ein AirPlay-Node läuft |
| Web-Interface | TCP 8443 (einstellbar) | **HTTPS**, nur wenn eingeschaltet (Standard: aus); nur private Adressen; Kopplung per Einmal-Code |

Es gibt keine Authentifizierung auf diesen Ports; nutze sie nur in vertrauenswürdigen privaten Netzen und erlaube sie in der Windows-Firewall **nur für „Privat“**.

## Web-Interface (Sicherheitsmodell)
Nur HTTPS (TLS 1.2+, Windows SChannel), kein HTTP-Port. Eigene lokale CA im Benutzerprofil (privater Schlüssel in einem Windows-Schlüsselbund des Benutzers). Verbindungen von nicht-privaten Adressen werden sofort geschlossen. Kopplung: 8-stelliger Einmal-Code (120 s), Sperre nach 5 Fehlversuchen je IP, Sitzungstoken 256 Bit (nur Hash gespeichert), Cookie `Secure; HttpOnly; SameSite=Strict`, CSRF-/Origin-Prüfung, CSP mit Hashes, WebSocket nur mit gültiger Sitzung, maskierte Frames Pflicht, Befehle begrenzt (60/s) und **im Router vollständig geprüft**. Protokoll: `docs/WEB-API.md`. Der Autostart schreibt nur einen Eintrag im Benutzer-Teil der Registry (HKCU\\…\\Run), ohne Administratorrechte; der Router läuft immer unter deinem Benutzerkonto, nicht als Systemdienst. Das Handy-Mikrofon wird nur zum Einmessen in den Arbeitsspeicher des Routers gestreamt und nicht gespeichert. Den Port **nicht** ins Internet weiterleiten; für unterwegs ein VPN nutzen.

## Schwachstellen melden
Bitte **nicht** als öffentliches Issue. Nutze „Security → Report a vulnerability“ (private Meldung) in diesem Repository.
Beschreibe kurz Auswirkung und Reproduktion. Du erhältst so schnell wie möglich eine Rückmeldung.

## Unterstützte Version
Jeweils die aktuelle Veröffentlichung.
