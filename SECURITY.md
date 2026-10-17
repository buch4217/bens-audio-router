# Sicherheit

Bens Audio Router öffnet bewusst **lokale Netzwerkdienste**, wenn die entsprechenden Funktionen genutzt werden:

| Funktion | Port | Hinweis |
|---|---|---|
| Sonos-Stream (HTTP) | TCP 8100 (einstellbar) | liefert den Audiostream an den Sonos |
| AirPlay-Empfänger | TCP 5000, UDP 6000–6200, UDP 5353 (mDNS) | nur wenn ein AirPlay-Node läuft |

Es gibt keine Authentifizierung auf diesen Ports; nutze sie nur in vertrauenswürdigen privaten Netzen und erlaube sie in der Windows-Firewall **nur für „Privat“**.

## Schwachstellen melden
Bitte **nicht** als öffentliches Issue. Nutze „Security → Report a vulnerability“ (private Meldung) in diesem Repository.
Beschreibe kurz Auswirkung und Reproduktion. Du erhältst so schnell wie möglich eine Rückmeldung.

## Unterstützte Version
Jeweils die aktuelle Veröffentlichung.
