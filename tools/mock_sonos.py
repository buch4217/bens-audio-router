#!/usr/bin/env python3
# Minimaler Sonos-Mock (UPnP/SOAP) + simulierter Player mit Vorpuffer, fuer Tests von Bens Audio Router
import http.server, socketserver, threading, time, re, socket, sys, html, urllib.parse, os
BIND = os.environ.get('BIND', '127.0.0.1')
CHUNK = os.environ.get('CHUNK') == '1'
def send_body(h, data, ctype=None):
    h.send_response(200)
    if ctype: h.send_header('Content-Type', ctype)
    if CHUNK:
        h.send_header('Transfer-Encoding', 'chunked'); h.end_headers()
        for i in range(0, len(data), 23):
            c = data[i:i+23]; h.wfile.write(('%x\r\n' % len(c)).encode() + c + b'\r\n')
        h.wfile.write(b'0\r\n\r\n')
    else:
        h.send_header('Content-Length', str(len(data))); h.end_headers(); h.wfile.write(data)
PREBUF = float(sys.argv[1]) if len(sys.argv) > 1 else 2.0     # Sekunden Audio, die der "Sonos" vor Start sammelt
NOPOS = len(sys.argv) > 2 and sys.argv[2] == 'nopos'
FLAC = len(sys.argv) > 3 and sys.argv[3] == 'flac'
REJECT = len(sys.argv) > 4 and sys.argv[4] == 'rejectflac'
BURST = 1.5
state = {'uri': None, 'playing': False, 'o': None, 'tfirst': None, 'recv': 0, 'vol': None, 'rate': 44100}
def log(*a): print('[mock %.3f]' % time.time(), *a, flush=True)
def player(uri):
    u = uri.replace('x-rincon-mp3radio://', 'http://')
    pu = urllib.parse.urlparse(u)
    s = socket.create_connection((pu.hostname, pu.port), 5)
    s.sendall(('GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n' % (pu.path, pu.netloc)).encode())
    buf = b''; hdr_done = False; first = None; data = 0
    t_conn = time.time()
    log('Player verbunden', u)
    while state['playing'] and state['uri'] == uri:
        d = s.recv(65536)
        if not d: break
        if not hdr_done:
            buf += d
            i = buf.find(b'\r\n\r\n')
            if i < 0: continue
            hdr = buf[:i].decode(errors='replace'); d = buf[i+4+(42 if FLAC else 44):]  # Header + WAV-Header (44)
            m = re.search(r'(\d+)\s*$', '')  # ignore
            hdr_done = True
            log('HTTP-Header:', hdr.splitlines()[0], '|', [l for l in hdr.splitlines() if 'Content-Type' in l])
            first = time.time()
            state['tfirst'] = first
        data += len(d); state['recv'] = data
        need = int(PREBUF * state['rate'] * 4)
        if state['o'] is None and ((FLAC and time.time() - first >= max(0.0, PREBUF - BURST)) or (not FLAC and data >= need)):
            state['o'] = time.time()
            log('Wiedergabe beginnt (Prebuffer %.2fs erreicht), %.3fs nach erstem Byte' % (PREBUF, state['o'] - first))
    s.close()
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def do_GET(self):
        if 'device_description' in self.path:
            body = ('<?xml version="1.0"?><root><device><roomName>Mock %s</roomName><modelName>Sonos Connect</modelName><UDN>uuid:RINCON_MOCK%s01400</UDN></device></root>' % (BIND, BIND.replace('.', ''))).encode()
            send_body(self, body)
        else:
            self.send_response(404); self.send_header('Content-Length', '0'); self.end_headers()
    def do_POST(self):
        n = int(self.headers.get('Content-Length', 0)); body = self.rfile.read(n).decode()
        act = self.headers.get('SOAPACTION', '').strip('"').split('#')[-1]
        resp = ''
        if act == 'SetAVTransportURI':
            uri = html.unescape(re.search(r'<CurrentURI>(.*?)</CurrentURI>', body).group(1))
            meta = re.search(r'<CurrentURIMetaData>(.*?)</CurrentURIMetaData>', body, re.S).group(1)
            log('SetAVTransportURI', uri, '| Meta enthaelt Titel:', 'dc:title' in html.unescape(meta))
            state['uri'] = uri; state['playing'] = False; state['o'] = None
            if REJECT and uri.endswith('.flac'):
                log('lehne FLAC ab (UPnP 714)'); out=b'<errorCode>714</errorCode>'; self.send_response(500); self.send_header('Content-Length', str(len(out))); self.end_headers(); self.wfile.write(out); return
        elif act == 'Play' and (state['uri'] or '').startswith('x-rincon:'):
            log('Play ignoriert (Gruppenmitglied)')
        elif act == 'Play':
            state['playing'] = True; state['o'] = None
            threading.Thread(target=player, args=(state['uri'],), daemon=True).start()
            log('Play')
        elif act == 'Stop':
            state['playing'] = False; log('Stop')
        elif act == 'GetMediaInfo':
            resp = '<CurrentURI>%s</CurrentURI>' % html.escape(state['uri'] or '')
        elif act == 'BecomeCoordinatorOfStandaloneGroup':
            state['uri'] = None; log('BecomeCoordinatorOfStandaloneGroup')
        elif act == 'GetTransportInfo':
            resp = '<CurrentTransportState>%s</CurrentTransportState>' % ('PLAYING' if state['playing'] else 'STOPPED')
        elif act == 'GetPositionInfo':
            if NOPOS or state['o'] is None: rel = 'NOT_IMPLEMENTED' if NOPOS else '0:00:00'
            else:
                t = max(0, int(time.time() - state['o'])); rel = '%d:%02d:%02d' % (t // 3600, t // 60 % 60, t % 60)
            resp = '<Track>1</Track><RelTime>%s</RelTime>' % rel
        elif act == 'GetVolume':
            resp = '<CurrentVolume>%s</CurrentVolume>' % (state['vol'] or '25')
        elif act == 'SetVolume':
            state['vol'] = re.search(r'<DesiredVolume>(\d+)', body).group(1); log('SetVolume', state['vol'])
        out = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:%sResponse>%s</u:%sResponse></s:Body></s:Envelope>' % (act, resp, act)).encode()
        send_body(self, out, 'text/xml')
class S(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads = True; allow_reuse_address = True
log('Mock-Sonos auf', BIND, ':1400, Prebuffer', PREBUF, 'nopos' if NOPOS else '')
S((BIND, 1400), H).serve_forever()
