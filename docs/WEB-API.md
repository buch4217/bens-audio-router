# Web-Interface – Protokoll und Sicherheitsmodell (Entwurf v3)

Status: **umgesetzt** (Version 4.3). Server-Kern `src/webcore.h` (getestet mit `tests/test_webcore.cpp` + `web/test/cpp-server.test.js`), Windows-Teil `src/webwin.h` (SChannel, lokale CA, Listener), QR `src/qr.h`, Referenz-Server `web/mock-server.js` (Node) und Web-App `web/index.html` (in die EXE eingebettet über `tools/gen_web_assets.py`).
Der Windows-Teil (TLS/CA/Dialog) läuft nur unter Windows; alles andere ist plattformunabhängig getestet.

## Vorschau am Rechner oder Handy
```
node web/mock-server.js --host 0.0.0.0 --port 8443     # Kopplungscode erscheint im Terminal
```
Dann `https://<IP-des-Rechners>:8443/` öffnen (Zertifikatswarnung bestätigen; ein Service Worker/„Zum Home-Bildschirm“ braucht ein **vertrauenswürdiges** Zertifikat).

## Endpunkte (nur HTTPS)
| Methode/Pfad | Auth | Zweck |
|---|---|---|
| `GET /` | – | Web-App (eine Datei, CSP mit Hashes, kein `unsafe-inline`) |
| `GET /manifest.webmanifest`, `/sw.js`, `/icon-*.png` | – | PWA |
| `GET /ca.crt` | – | öffentliches CA-Zertifikat zum Installieren auf dem Handy |
| `POST /api/pair` `{code,name}` | – | Kopplung; setzt Cookie `bar_sid` (Secure, HttpOnly, SameSite=Strict, 30 Tage) |
| `POST /api/logout` | Cookie | Gerät abmelden |
| `GET /api/state` | Cookie | Gesamtzustand (`401` = nicht gekoppelt) |
| `GET /ws` (Upgrade) | Cookie | WebSocket für Pegel, Zustand, Befehle |

POST-Anfragen brauchen den Header `X-BAR: 1`; ein vorhandener `Origin` muss `https://<Host>` sein (CSRF-Schutz). WebSocket: `Origin` (falls gesendet) muss passen, Client-Frames müssen maskiert sein, keine Fragmente, max. 8 gleichzeitige Verbindungen, max. 60 Befehle/s je Verbindung.

## Kopplung
8-stelliger Einmal-Code, in der Windows-App erzeugt und angezeigt (QR-Code: `https://<host>:<port>/#code=<8 Ziffern>`), **120 s gültig, einmal verwendbar**. Nach 5 Fehlversuchen je IP: `429` für 60 s (auch mit richtigem Code). Sitzungstoken: 256 Bit zufällig (OS-CSPRNG), im Router nur als Hash speichern. Geräteliste mit Abmelden (`revoke`).

## WebSocket-Nachrichten
Server → Client
- `{"t":"state", ver, running, autopilot, nodes:[…], conns:[…], scenes:[…], devices:[…]}` bei jeder Strukturänderung (Verbindung, Gain, Mute, Layout …)
- `{"t":"m","a":[[nodeId, dbL, dbR, clip], …]}` Pegel, 20 Hz
- `{"t":"ack","id":N,"ok":bool,"err":""}` Antwort auf jeden Befehl

Node: `{id, kind, name, x, y, ports:"in"|"out"|"both", gain, mute, delay, status, sumMono, zones:[{name,vol}], rec:{on,sec,bytes,fmt}}` ·
Kabel: `{id, src, dst, srcCh, dstCh, lag}` (`srcCh`: 0 Stereo, 1 L, 2 R, 3 Mono-Summe · `dstCh`: 0 Stereo, 1 L, 2 R, 3 Mitte; nur relevant bei Ziel = Summen-Modul)

Client → Server: `{"t":"cmd","id":N,"op":…}`
| op | Felder | Regeln (serverseitig geprüft) |
|---|---|---|
| `gain` | node, db | −60 … **0** (kein Pluspegel) |
| `mute` | node, on | Boolean |
| `autopilot` | on | Boolean |
| `rec` | node, on | nur Aufnahme-Box; Start nur mit verbundener Quelle |
| `connect` | src, dst | Quelle hat Ausgang, Ziel Eingang, nicht Summe→Summe, nicht doppelt; Standardzuordnung am Summen-Modul: Mono-Quellen erst links, dann rechts, danach Mitte |
| `disconnect` | conn | |
| `route` | conn, srcCh, dstCh | nur Kabel in ein Summen-Modul |
| `sumMono` | node, on | nur Summen-Modul |
| `zoneVol` | node, zone, vol | 0 … 1, nur Sonos-Node |
| `scene` / `sceneSave` | name | Layout laden/speichern (Name wird bereinigt) |
| `trim` | node, ms | −500 … +500, nur Ausgänge/Sonos |
| `delay` | node, ms | 0 … 2000; an Ausgängen nur bei ausgeschaltetem Autopilot |
| `addNode` | kind (`cap`/`loop`/`out`/`gen`/`air`/`rec`/`sum`), dev | `dev` = Geräte-ID aus `hw` bzw. `click`/`sine`/`pink`; max. 40 Nodes |
| `removeNode` | node | |
| `micStart` / `micStop` | rate (8000–96000) | Handy-Mikrofon als Quelle an-/abmelden |
| `calibrate` | – | startet die Einmessung (Handy-Mikrofon muss Daten liefern) |
| `engine` | on | Engine Start/Stopp (wie die Taste „Start“) |
| `act` | name: `rescan` `sonosSearch` `calibrateLocal` `vdevSetup` `vdevRestore` | Aktionen wie in der Desktop-App |
| `global` | key: `pad` `base` `poll` `drv`, value | nur Werte aus `global.opts` |
| `setProp` | node, key, value | je Node-Art: Sonos-Stream `ip` `port` `scheme` `rate` `burst` `fmt` `volCtl` `dither` `autoLat` `vol` `play`; AirPlay `name` `port` `buf` `latAuto` `latMs` `useVol`; Aufnahme `fmt` `rate` `dir`; Test-Signal `gen`; Ausgänge `ext` |
| `addStream` | ip (optional) | Sonos-Stream anlegen (Zone aus `sonosFound`) |
| `sonosPick` / `groupAdd` | node, ip | Zone wählen / zur Gruppe hinzufügen |
| `groupClear` / `reconnect` | node | Gruppe leeren / Node neu verbinden |
| `move` | node, x, y | Position im Graph |
| `sceneDelete` | name | Layout löschen |
| `revoke` | device | Gerät abmelden, dessen Verbindung wird getrennt |

Weitere Zustandsfelder (4.5): `engine:{running}`, `global:{pad,base,poll,drv,opts}`, `sonosFound:[{ip,name,model}]`, `msg` (Statusmeldung der App), je Node `p` (Einstellungen der Node-Art), Ausgänge zusätzlich `ext` und `lat:{total,err,src}`.

Zusätzliche Zustandsfelder: Node `trim`, `dly` (Delay ohne Trim); `hw:{cap:[{id,name}],out:[…]}` (Geräte-Listen), `mic:{on,exists}`, `cal:{busy,msg}`.

**Binäre Frames (Handy-Mikrofon):** `[u8 1][3 Byte 0][u32 Rate LE][u64 Startsample LE][int16 LE Mono …]`, max. 16 KB je Frame, 4–8192 Samples. Der Router bildet daraus die Quelle „Handy-Mikrofon“ und gleicht die Zeitachse mit einem Minimum-Filter (kleinste Laufzeit) ab; für das Einmessen zählen nur Unterschiede zwischen Ausgängen.

Wichtig: **Der Server vertraut dem Client nie** – alle Regeln gelten im Router, die Web-App prüft nur zur Bequemlichkeit.

## Zertifikate (so macht es der Router)
- Beim ersten Start eigene **lokale CA** erzeugen (RSA 2048/ECDSA P-256, 10 Jahre) und damit ein **Server-Zertifikat** (SAN: Hostname, `bens-audio-router.local`, aktuelle IPs; 1 Jahr, bei IP-Wechsel neu ausstellen) signieren. Privater CA-Schlüssel bleibt im Benutzerprofil des Routers.
- Handy installiert **einmal** die CA (`/ca.crt`): Android: Einstellungen → Zertifikat installieren; iOS: Profil laden, danach unter „Info → Zertifikatsvertrauen“ aktivieren.
- Optional: eigenes Zertifikat importieren (PFX/PEM), z. B. von einer echten Domain.
- Nur TLS 1.2+, **kein HTTP-Port**.
- Technik: Windows **SChannel** (kein Fremdcode), Zertifikate über CryptoAPI/CNG.
