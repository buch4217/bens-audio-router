#!/usr/bin/env node
// Referenz-/Mock-Server fuer das Web-Interface von Bens Audio Router (nur Node-Bordmittel, kein npm).
// Definiert das Protokoll (siehe docs/WEB-API.md) und dient als Testgegenstelle fuer die Web-App.
//   node mock-server.js [--port 8443] [--host 127.0.0.1|0.0.0.0] [--code 12345678] [--code-ttl 120]
'use strict';
const https = require('https'), fs = require('fs'), path = require('path'), crypto = require('crypto'), os = require('os'), cp = require('child_process');
const A = process.argv.slice(2), opt = (k, d) => { const i = A.indexOf('--' + k); return i >= 0 ? A[i + 1] : d };
const PORT = +opt('port', 8443), CODE_TTL = +opt('code-ttl', 120) * 1000, MAXWS_ = 8;
const dir = __dirname, INDEX = fs.readFileSync(path.join(dir, 'index.html'));
const sha = b => "'sha256-" + crypto.createHash('sha256').update(b).digest('base64') + "'";
const html = INDEX.toString('utf8'), SCRIPT_H = sha(html.match(/<script>([\s\S]*)<\/script>/)[1]), STYLE_H = sha(html.match(/<style>([\s\S]*)<\/style>/)[1]);
const SW = `const C='bar-shell-v4';self.addEventListener('install',e=>{e.waitUntil(caches.open(C).then(c=>c.addAll(['/','/manifest.webmanifest','/icon-192.png'])));self.skipWaiting()});self.addEventListener('activate',e=>e.waitUntil(clients.claim()));self.addEventListener('fetch',e=>{const u=new URL(e.request.url);if(u.pathname.startsWith('/api')||u.pathname==='/ws')return;e.respondWith(fetch(e.request).catch(()=>caches.match(e.request)))});`;
const MANIFEST = JSON.stringify({ name: 'Bens Audio Router', short_name: 'Audio Router', start_url: '/', display: 'standalone', background_color: '#181A20', theme_color: '#181A20', icons: [{ src: '/icon-192.png', sizes: '192x192', type: 'image/png' }, { src: '/icon-512.png', sizes: '512x512', type: 'image/png' }] });

// ---------------------------------------------------------------- Modell (im echten Router: die Node-/Kabel-Listen der App)
const mk = (id, kind, name, x, y, ports, extra = {}) => ({ id, kind, name, x, y, ports, gain: 0, mute: false, delay: 0, dly: 0, trim: 0, status: '', sumMono: false, ...extra });
const M = {
  nodes: [mk(1, 'cap', 'Xone:92 · Master', 50, 140, 'out', { status: '48000 Hz', stereo: true }), mk(2, 'cap', 'Mikrofon · Mono 1', 50, 290, 'out', { status: '48000 Hz', stereo: false }), mk(3, 'cap', 'Aux · Mono 2', 50, 440, 'out', { status: '48000 Hz', stereo: false }),
    mk(4, 'loop', 'Player · Virtual 3/4', 50, 590, 'out', { status: '48000 Hz', stereo: true }), mk(5, 'sum', 'Summe (Mono → Stereo)', 410, 365, 'both', { status: 'Summe' }),
    mk(6, 'out', 'DCX2496 A · Mains', 770, 140, 'in', { status: '48000 Hz' }), mk(7, 'out', 'DCX2496 B · Subs', 770, 290, 'in', { status: '48000 Hz' }), mk(8, 'out', 'DCX2496 C · Fill', 770, 440, 'in', { status: '48000 Hz' }),
    mk(9, 'stream', 'Stream: Lounge (Sonos)', 1130, 140, 'in', { status: 'spielt · 1833 ms (Position)', p: { port: 8100, scheme: 0, rate: 44100, burst: 2000, fmt: 0, volCtl: false, dither: false, autoLat: true, vol: 0, play: true, ctl: 0, ip: '192.168.0.21', room: 'Lounge', group: [] }, zones: [{ name: 'Wohnzimmer', vol: .25 }, { name: 'Schlafzimmer', vol: .25 }, { name: 'Küche', vol: .25 }] }),
    mk(10, 'rec', 'Aufnahme-Box', 1130, 600, 'in', { status: 'bereit', rec: { on: false, sec: 0, bytes: 0, parts: 0, ch: 2, lufs: -120, tp: -120, gain: 0, gr: 0, clips: 0, ring: 0, pre: 0, fmt: 'FLAC 24 Bit · 48000 Hz' }, p: { fmt: 1, rate: 48000, dir: os.tmpdir(), dither: -1, mt: 0, bp: false, pre: 0, split: 0, al: { mode: 2, target: -18, boost: 24, ceil: -1, speed: 1 } } })],
  conns: [], nextConn: 1, engine: { running: true }, live: { on: false, auto: true, ms: 8, lag: 8, raises: 0, under: 0, resync: 0 }, vb: { on: false, layout: 0, installed: false, daw: false }, g: { pad: 30, base: 50, poll: 2, drv: 50 }, msg: '', mic: { on: false, exists: false }, cal: { busy: false, msg: '' }, nextNode: 100, autopilot: false, scenes: { 'Party': { c: [[1, 6], [1, 7], [4, 6], [2, 5], [3, 5], [5, 8], [1, 9]], g: {} }, 'Heimkino': { c: [[1, 9], [4, 9]], g: { 9: -3 } } }, devices: new Map(), t0: Date.now()
};
const LAYOUTS = [['8 × Stereo', [[0, 2, 0, 'Stereo 1'], [2, 2, 0, 'Stereo 2'], [4, 2, 0, 'Stereo 3'], [6, 2, 0, 'Stereo 4'], [8, 2, 0, 'Stereo 5'], [10, 2, 0, 'Stereo 6'], [12, 2, 0, 'Stereo 7'], [14, 2, 0, 'Stereo 8']]],
  ['5.1 + 5 × Stereo', [[0, 6, 2, '5.1'], [6, 2, 0, 'Stereo 4'], [8, 2, 0, 'Stereo 5'], [10, 2, 0, 'Stereo 6'], [12, 2, 0, 'Stereo 7'], [14, 2, 0, 'Stereo 8']]],
  ['7.1 + 4 × Stereo', [[0, 8, 3, '7.1'], [8, 2, 0, 'Stereo 5'], [10, 2, 0, 'Stereo 6'], [12, 2, 0, 'Stereo 7'], [14, 2, 0, 'Stereo 8']]],
  ['5.1 × 2 + 2 × Stereo', [[0, 6, 2, '5.1 A'], [6, 6, 2, '5.1 B'], [12, 2, 0, 'Stereo 7'], [14, 2, 0, 'Stereo 8']]],
  ['7.1 × 2', [[0, 8, 3, '7.1 A'], [8, 8, 3, '7.1 B']]],
  ['Quad × 4', [[0, 4, 1, 'Quad 1'], [4, 4, 1, 'Quad 2'], [8, 4, 1, 'Quad 3'], [12, 4, 1, 'Quad 4']]],
  ['7.1.4 (Atmos-Bett) + 2 × Stereo', [[0, 12, 4, '7.1.4'], [12, 2, 0, 'Stereo 7'], [14, 2, 0, 'Stereo 8']]]];
const CHN = [['L', 'R'], ['L', 'R', 'Ls', 'Rs'], ['L', 'R', 'C', 'LFE', 'Ls', 'Rs'], ['L', 'R', 'C', 'LFE', 'Ls', 'Rs', 'Lrs', 'Rrs'], ['L', 'R', 'C', 'LFE', 'Lss', 'Rss', 'Lrs', 'Rrs', 'Ltf', 'Rtf', 'Ltr', 'Rtr']];
const chanNames = ly => { const o = []; for (const [f, c, t, n] of LAYOUTS[ly][1]) for (let i = 0; i < c; i++) o[f + i] = n + ' ' + CHN[t][i]; return o };
const REC_RATES = [44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000];
const vbusState = () => ({ on: M.vb.on, open: M.vb.on, layout: M.vb.layout, installed: M.vb.installed, daw: M.vb.daw, rate: M.vb.daw ? 48000 : 0, buf: M.vb.daw ? 256 : 0, layouts: LAYOUTS.map(l => l[0]), groups: LAYOUTS[M.vb.layout][1].map(([first, count, type, name]) => ({ first, count, type, name })), channels: chanNames(M.vb.layout) });
const vname = (kind, idx) => idx >= 100 ? 'DAW ' + LAYOUTS[M.vb.layout][1][idx - 100][3] + ' → Stereo (Downmix)' : (kind === 'vin' ? 'DAW-Ausgang ' : 'DAW-Eingang ') + (2 * idx + 1) + '/' + (2 * idx + 2);
const GOPTS = { pad: [10, 20, 30, 50, 100], base: [10, 20, 30, 50, 100, 200, 500], poll: [1, 2, 3, 5], drv: [20, 50, 100, 200] };
const DLY = { 6: 1783, 7: 1783, 8: 1768 };
for (const n of M.nodes) if (n.kind === 'out' || n.kind === 'cap' || n.kind === 'loop') n.p = { excl: false, exRate: 0, pair: 0, liveDirect: true, mode: 'std', per: 10, lat: 12 };
const byId = id => M.nodes.find(n => n.id === id), hasOut = n => n.ports === 'out' || n.ports === 'both', hasIn = n => n.ports === 'in' || n.ports === 'both';
function canConnect(a, b) { const A = byId(a), B = byId(b); if (!A || !B) return 'Node fehlt'; if (a === b) return 'Nicht mit sich selbst'; if (!hasOut(A)) return A.name + ' hat keinen Ausgang'; if (!hasIn(B)) return B.name + ' hat keinen Eingang'; if (A.kind === 'sum' && B.kind === 'sum') return 'Summen-Module lassen sich nicht hintereinander schalten'; if (M.conns.some(c => c.src === a && c.dst === b)) return 'Schon verbunden'; return '' }
function addConn(a, b) { const c = { id: M.nextConn++, src: a, dst: b, srcCh: 0, dstCh: 0, lag: 30 }; if (byId(b).kind === 'sum') { const n = M.conns.filter(x => x.dst === b).length; if (!byId(a).stereo) { c.srcCh = 3; c.dstCh = n === 0 ? 1 : n === 1 ? 2 : 3 } } M.conns.push(c); return c }
function sinkExtras() { for (const n of M.nodes) if ((n.kind === 'out' || n.kind === 'stream') && !n.lat) { n.ext = 0; n.lat = { total: 30, err: 5, src: 'Schätzung' } } }
function applyAutopilot() { sinkExtras(); for (const n of M.nodes) { if (n.kind === 'out' && M.autopilot) n.dly = DLY[n.id] || 0; n.delay = (n.dly || 0) + (n.trim || 0) } for (const c of M.conns) c.lag = 30 + (byId(c.dst).kind === 'out' ? byId(c.dst).delay : 0) }
function loadScene(name) { const s = M.scenes[name]; if (!s) return false; M.conns = []; for (const [a, b] of s.c) addConn(a, b); for (const n of M.nodes) n.gain = s.g[n.id] || 0; applyAutopilot(); return true }
const publicNode = n => { const o = { ...n }; delete o.stereo; return o };
const stateFor = dev => ({ t: 'state', ver: '3.0', running: true, autopilot: M.autopilot, nodes: M.nodes.map(publicNode), conns: M.conns, scenes: Object.keys(M.scenes), engine: M.engine, global: { pad: M.g.pad, base: M.g.base, poll: M.g.poll, drv: M.g.drv, opts: GOPTS, live: { ...M.live, opts: [2, 3, 4, 6, 8, 12, 16, 24, 32, 40] }, vbus: vbusState() }, sonosFound: [{ ip: '192.168.0.21', name: 'Wohnzimmer', model: 'Sonos Connect' }, { ip: '192.168.0.22', name: 'Küche', model: 'Sonos One' }], msg: M.msg, hw: { cap: [{ id: 'cap1', name: 'Mikrofon (USB)' }, { id: 'cap2', name: 'Line (MiniFuse 1)' }], out: [{ id: 'out1', name: 'DCX2496 A' }, { id: 'out2', name: 'Lautsprecher' }] }, mic: { on: M.mic.on, exists: M.mic.exists }, cal: M.cal, devices: [...M.devices.values()].map(d => ({ id: d.id, name: d.name, me: d.id === dev?.id })) });

// ---------------------------------------------------------------- Sitzungen, Kopplung, Ratenbegrenzung
const sessions = new Map(), fails = new Map(); let pair = null;
function newCode() { pair = { code: opt('code', String(crypto.randomInt(0, 1e8)).padStart(8, '0')), exp: Date.now() + CODE_TTL }; return pair.code }
function cookieOf(req) { const m = /(?:^|;\s*)bar_sid=([A-Za-z0-9_-]{43})/.exec(req.headers.cookie || ''); return m && m[1] }
const auth = req => { const t = cookieOf(req); const s = t && sessions.get(t); return s && s.exp > Date.now() ? s : null };
function throttled(ip) { const f = fails.get(ip); return f && f.n >= 5 && Date.now() < f.until }
function fail(ip) { const f = fails.get(ip) || { n: 0, until: 0 }; f.n++; if (f.n >= 5) f.until = Date.now() + 60000; fails.set(ip, f) }
const eq = (a, b) => { const x = Buffer.from(String(a)), y = Buffer.from(String(b)); return x.length === y.length && crypto.timingSafeEqual(x, y) };

// ---------------------------------------------------------------- HTTP
function hdr(req, extra = {}) { const host = req.headers.host || 'localhost'; return { 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer', 'X-Frame-Options': 'DENY', 'Cross-Origin-Opener-Policy': 'same-origin', 'Cache-Control': 'no-store',
  'Content-Security-Policy': `default-src 'none'; script-src ${SCRIPT_H}; style-src ${STYLE_H}; img-src 'self' data:; connect-src 'self' wss://${host}; manifest-src 'self'; worker-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'`, ...extra } }
function body(req, max = 4096) { return new Promise((res, rej) => { let n = 0; const ch = []; req.on('data', c => { n += c.length; if (n > max) { rej(new Error('zu gross')); req.destroy() } else ch.push(c) }); req.on('end', () => res(Buffer.concat(ch).toString())); req.on('error', rej) }) }
const json = (req, res, code, o, extra) => { res.writeHead(code, hdr(req, { 'Content-Type': 'application/json', ...extra })); res.end(JSON.stringify(o)) };
async function handler(req, res) {
  const url = new URL(req.url, 'https://x'), p = url.pathname, ip = req.socket.remoteAddress;
  if (req.method === 'GET') {
    if (p === '/') { res.writeHead(200, hdr(req, { 'Content-Type': 'text/html; charset=utf-8' })); return res.end(INDEX) }
    if (p === '/manifest.webmanifest') { res.writeHead(200, hdr(req, { 'Content-Type': 'application/manifest+json' })); return res.end(MANIFEST) }
    if (p === '/sw.js') { res.writeHead(200, hdr(req, { 'Content-Type': 'text/javascript', 'Cache-Control': 'no-cache' })); return res.end(SW) }
    if (['/icon-32.png', '/icon-180.png', '/icon-192.png', '/icon-512.png', '/apple-touch-icon.png', '/favicon.ico'].includes(p)) { res.writeHead(200, hdr(req, { 'Content-Type': p.endsWith('.ico') ? 'image/x-icon' : 'image/png', 'Cache-Control': 'max-age=86400' })); return res.end(fs.readFileSync(path.join(dir, p.slice(1)))) }
    if (p === '/api/state') { const s = auth(req); if (!s) return json(req, res, 401, { err: 'nicht gekoppelt' }); return json(req, res, 200, stateFor(s)) }
    if (p === '/ca.crt') { res.writeHead(200, hdr(req, { 'Content-Type': 'application/x-x509-ca-cert', 'Content-Disposition': 'attachment; filename="bens-audio-router-ca.crt"' })); return res.end(CERT_PEM) }
  } else if (req.method === 'POST') {
    if (req.headers['x-bar'] !== '1' || (req.headers.origin && req.headers.origin !== 'https://' + req.headers.host)) return json(req, res, 403, { err: 'verboten' });   // CSRF-Schutz
    if (p === '/api/pair') {
      if (throttled(ip)) return json(req, res, 429, { err: 'zu viele Versuche' }, { 'Retry-After': '60' });
      let b; try { b = JSON.parse(await body(req)) } catch { return json(req, res, 400, { err: 'ungueltig' }) }
      if (!pair || Date.now() > pair.exp || !eq(b.code || '', pair.code)) { fail(ip); return json(req, res, 401, { err: 'Code falsch oder abgelaufen' }) }
      pair = null; fails.delete(ip);                                                                   // Einmal-Code
      const tok = crypto.randomBytes(32).toString('base64url'), id = crypto.randomBytes(6).toString('hex'), name = String(b.name || 'Geraet').slice(0, 40).replace(/[<>"&]/g, '');
      sessions.set(tok, { id, name, exp: Date.now() + 30 * 864e5 }); M.devices.set(id, { id, name, tok });
      return json(req, res, 200, { ok: true }, { 'Set-Cookie': `bar_sid=${tok}; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=${30 * 86400}` });
    }
    if (p === '/api/logout') { const t = cookieOf(req); if (t && sessions.has(t)) { M.devices.delete(sessions.get(t).id); sessions.delete(t); broadcastState() } return json(req, res, 200, { ok: true }, { 'Set-Cookie': 'bar_sid=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0' }) }
  }
  json(req, res, 404, { err: 'nicht gefunden' });
}

// ---------------------------------------------------------------- WebSocket (RFC 6455, nur noetiges)
const clients = new Set();
function frame(op, payload) { const n = payload.length, h = n < 126 ? Buffer.from([0x80 | op, n]) : n < 65536 ? Buffer.from([0x80 | op, 126, n >> 8, n & 255]) : null; return Buffer.concat([h, payload]) }
const sendWs = (c, o) => { if (!c.socket.destroyed) c.socket.write(frame(1, Buffer.from(JSON.stringify(o)))) };
function broadcastState() { for (const c of clients) sendWs(c, stateFor(c.sess)) }
function upgrade(req, socket) {
  const s = auth(req), host = req.headers.host, o = req.headers.origin;
  if (req.url !== '/ws' || !s || (o && o !== 'https://' + host) || clients.size >= MAXWS_ || !/websocket/i.test(req.headers.upgrade || '')) { socket.end('HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n'); return }
  const key = req.headers['sec-websocket-key']; const acc = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${acc}\r\n\r\n`);
  const c = { socket, sess: s, buf: Buffer.alloc(0) }; clients.add(c); socket.setNoDelay(true); sendWs(c, stateFor(s));
  socket.on('data', d => { c.buf = Buffer.concat([c.buf, d]); if (c.buf.length > 70000) return socket.destroy(); let f; while ((f = parseFrame(c))) { if (f.op === 8) { socket.end(frame(8, Buffer.alloc(0))); return } if (f.op === 9) socket.write(frame(10, f.data)); if (f.op === 1) onMsg(c, f.data.toString()); else if (f.op === 2) c.binBytes = (c.binBytes || 0) + f.data.length } });
  socket.on('close', () => clients.delete(c)); socket.on('error', () => clients.delete(c));
}
function parseFrame(c) {
  const b = c.buf; if (b.length < 2) return null; const fin = b[0] & 0x80, op = b[0] & 15, masked = b[1] & 0x80; let n = b[1] & 127, o = 2;
  if (!masked || !fin || (op > 2 && op < 8)) { c.socket.destroy(); return null }                        // Client-Frames muessen maskiert sein; keine Fragmente
  if (n === 126) { if (b.length < 4) return null; n = b.readUInt16BE(2); o = 4 } else if (n === 127) { c.socket.destroy(); return null }
  if (n > (op === 2 ? 16384 : 4096)) { c.socket.destroy(); return null }
  if (b.length < o + 4 + n) return null; const m = b.subarray(o, o + 4), d = Buffer.from(b.subarray(o + 4, o + 4 + n)); for (let i = 0; i < n; i++) d[i] ^= m[i & 3];
  c.buf = b.subarray(o + 4 + n); return { op, data: d }
}
const num = (v, a, b) => typeof v === 'number' && Number.isFinite(v) && v >= a && v <= b;
function exec(c, m) {
  const node = () => byId(m.node), bad = e => ({ ok: false, err: e });
  switch (m.op) {
    case 'gain': if (!node() || !num(m.db, -60, 0)) return bad('Ungueltig'); node().gain = m.db; return { ok: true };
    case 'mute': if (!node() || typeof m.on !== 'boolean') return bad('Ungueltig'); node().mute = m.on; return { ok: true };
    case 'autopilot': if (typeof m.on !== 'boolean') return bad('Ungueltig'); M.autopilot = m.on; applyAutopilot(); return { ok: true };
    case 'rec': { const n = node(); if (!n || n.kind !== 'rec' || typeof m.on !== 'boolean') return bad('Keine Aufnahme-Box'); if (m.on && !M.conns.some(x => x.dst === n.id)) return bad('Erst Quellen verbinden'); if (m.on && !n.rec.on) { n.rec.t0 = Date.now() } n.rec.on = m.on; if (!m.on) { n.status = 'bereit · gespeichert' } return { ok: true } }
    case 'connect': { const e = canConnect(m.src, m.dst); if (e) return bad(e); addConn(m.src, m.dst); applyAutopilot(); return { ok: true } }
    case 'disconnect': { const i = M.conns.findIndex(x => x.id === m.conn); if (i < 0) return bad('Verbindung fehlt'); M.conns.splice(i, 1); return { ok: true } }
    case 'route': { const x = M.conns.find(y => y.id === m.conn); if (!x || byId(x.dst).kind !== 'sum' || ![0, 1, 2, 3].includes(m.srcCh) || ![0, 1, 2, 3].includes(m.dstCh)) return bad('Ungueltig'); x.srcCh = m.srcCh; x.dstCh = m.dstCh; return { ok: true } }
    case 'sumMono': if (!node() || node().kind !== 'sum' || typeof m.on !== 'boolean') return bad('Kein Summen-Modul'); node().sumMono = m.on; return { ok: true };
    case 'zoneVol': { const n = node(); if (!n || !n.zones || !n.zones[m.zone] || !num(m.vol, 0, 1)) return bad('Ungueltig'); n.zones[m.zone].vol = m.vol; return { ok: true } }
    case 'scene': return typeof m.name === 'string' && loadScene(m.name) ? { ok: true } : bad('Layout fehlt');
    case 'sceneSave': { const nm = String(m.name || '').trim().replace(/[^\w .äöüÄÖÜß-]/g, '').slice(0, 40); if (!nm) return bad('Name fehlt'); M.scenes[nm] = { c: M.conns.map(x => [x.src, x.dst]), g: Object.fromEntries(M.nodes.filter(n => n.gain).map(n => [n.id, n.gain])) }; return { ok: true } }
    case 'trim': { const n = node(); if (!n || n.kind !== 'out' && n.kind !== 'stream' || !num(m.ms, -500, 500)) return bad('Ungueltig'); n.trim = Math.round(m.ms * 10) / 10; applyAutopilot(); return { ok: true } }
    case 'delay': { const n = node(); if (!n || n.kind === 'rec' || n.kind === 'sum' || !num(m.ms, 0, 2000)) return bad('Ungueltig'); if (n.kind === 'out' && M.autopilot) return bad('Autopilot ist an – bitte Feinkorrektur (Trim) nutzen'); n.dly = Math.round(m.ms * 10) / 10; applyAutopilot(); return { ok: true } }
    case 'addNode': { if (M.nodes.length >= 60) return bad('Zu viele Nodes'); const k = m.kind, d = String(m.dev || ''); let n = null; const id = M.nextNode++;
      if (k === 'vin' || k === 'vout') { let which = -1; if (d !== '' && d !== 'all') { which = d === 'down' ? 99 : parseInt(d, 10); if (!(which === 99 || (which >= 0 && which < 8) || (which >= 100 && which < 108))) return bad('Ungueltiger Kanal') } M.vb.on = true; const L = LAYOUTS[M.vb.layout][1]; const want = which === -1 ? [0, 1, 2, 3, 4, 5, 6, 7] : which === 99 ? (k === 'vin' ? L.map((g, i) => g[2] !== 0 ? 100 + i : -1).filter(x => x >= 0) : []) : (which >= 100 && (which - 100 >= L.length || k !== 'vin') ? [] : [which]); let added = 0;
        for (const idx of want) { if (M.nodes.some(x => x.kind === k && x.p && x.p.idx === idx)) continue; M.nodes.push(mk(M.nextNode++, k, vname(k, idx), k === 'vin' ? 50 : 770, 100 + 60 * (idx % 100), k === 'vin' ? 'out' : 'in', { p: { idx, liveDirect: true } })); added++ } return added ? { ok: true } : bad('Schon vorhanden') }
      if (k === 'cap' && ['cap1', 'cap2'].includes(d)) n = mk(id, 'cap', 'Eingang: ' + d, 50, 600, 'out'); else if (k === 'loop' && ['out1', 'out2'].includes(d)) n = mk(id, 'loop', 'Mitschnitt: ' + d, 50, 700, 'out'); else if (k === 'out' && ['out1', 'out2'].includes(d)) n = mk(id, 'out', d, 770, 700, 'in');
      else if (k === 'gen' && ['click', 'sine', 'pink'].includes(d)) n = mk(id, 'gen', 'Test: ' + d, 50, 800, 'out', { p: { gen: ['click', 'sine', 'pink'].indexOf(d) } }); else if (k === 'air') n = mk(id, 'air', 'AirPlay', 50, 900, 'out', { p: { port: 7000, buf: 500, latAuto: true, latMs: 0, useVol: false, name: 'Bens Audio Router' } }); else if (k === 'rec') n = mk(id, 'rec', 'Aufnahme-Box', 1130, 700, 'in', { rec: { on: false, sec: 0, bytes: 0, fmt: 'FLAC' } }); else if (k === 'sum') n = mk(id, 'sum', 'Summe', 410, 600, 'both');
      if (!n) return bad('Geraet nicht gefunden'); M.nodes.push(n); return { ok: true } }
    case 'removeNode': { const i = M.nodes.findIndex(n => n.id === m.node); if (i < 0) return bad('Node fehlt'); M.conns = M.conns.filter(c => c.src !== m.node && c.dst !== m.node); M.nodes.splice(i, 1); return { ok: true } }
    case 'micStart': { if (!num(m.rate, 8000, 96000) || m.rate !== Math.floor(m.rate)) return bad('Ungueltig'); M.mic = { on: true, exists: true }; return { ok: true } }
    case 'micStop': M.mic = { on: false, exists: false }; M.cal = { busy: false, msg: '' }; return { ok: true };
    case 'calibrate': { if (!M.mic.on) return bad('Handy-Mikrofon liefert keine Daten'); if (M.cal.busy) return bad('Einmessung laeuft bereits'); M.cal = { busy: true, msg: 'Einmessung laeuft …' }; setTimeout(() => { M.cal = { busy: false, msg: 'Fertig: 3 Ausgaenge ausgeglichen.' }; M.autopilot = true; applyAutopilot(); broadcastState() }, 300); return { ok: true } }
    case 'live': { const has = k => m[k] !== undefined; if (!has('on') && !has('auto') && !has('ms')) return bad('Ungueltig'); if (has('on') && typeof m.on !== 'boolean') return bad('Ungueltig'); if (has('auto') && typeof m.auto !== 'boolean') return bad('Ungueltig');
      if (has('ms') && !(Number.isInteger(m.ms) && m.ms >= 2 && m.ms <= 40)) return bad('Puffer 2–40 ms'); if (has('on')) M.live.on = m.on; if (has('auto')) M.live.auto = m.auto; if (has('ms')) { M.live.ms = m.ms; M.live.lag = m.ms } return { ok: true } }
    case 'vbus': { const has = k => m[k] !== undefined; if (!has('on') && !has('layout')) return bad('Ungueltig'); if (has('on') && typeof m.on !== 'boolean') return bad('Ungueltig'); if (has('layout') && !(Number.isInteger(m.layout) && m.layout >= 0 && m.layout < LAYOUTS.length)) return bad('Ungueltige Surround-Zuordnung');
      if (has('on')) M.vb.on = m.on; if (has('layout')) { M.vb.layout = m.layout; for (const n of M.nodes) if (n.kind === 'vin' || n.kind === 'vout') { if (n.p.idx >= 100 && n.p.idx - 100 >= LAYOUTS[m.layout][1].length) n.p.idx = 100; n.name = vname(n.kind, n.p.idx) } } return { ok: true } }
    case 'engine': if (typeof m.on !== 'boolean') return bad('Ungueltig'); M.engine.running = m.on; return { ok: true };
    case 'act': { const a = m.name; if (a === 'driverInstall' || a === 'driverRemove') { M.vb.installed = a === 'driverInstall'; M.msg = a; return { ok: true } } if (a === 'rescan' || a === 'sonosSearch' || a === 'vdevSetup' || a === 'vdevRestore') { M.msg = a; return { ok: true } } if (a === 'calibrateLocal') { if (!M.engine.running) return bad('Erst Start druecken'); M.cal = { busy: false, msg: 'Fertig (PC-Mikrofon)' }; return { ok: true } } return bad('Unbekannte Aktion') }
    case 'global': { const o = GOPTS[m.key]; if (!o || !o.includes(m.value)) return bad('Ungueltiger Wert'); M.g[m.key] = m.value; return { ok: true } }
    case 'reconnect': return node() ? { ok: true } : bad('Node fehlt');
    case 'move': { if (!node() || !num(m.x, -3000, 9000) || !num(m.y, -3000, 9000)) return bad('Ungueltig'); node().x = m.x; node().y = m.y; return { ok: true } }
    case 'sceneDelete': { if (typeof m.name !== 'string' || !M.scenes[m.name]) return bad('Layout fehlt'); delete M.scenes[m.name]; return { ok: true } }
    case 'addStream': { const id = M.nextNode++; M.nodes.push(mk(id, 'stream', 'Stream: ' + (m.ip || 'Sonos'), 1130, 800, 'in', { p: { port: 8100 + id, scheme: 0, rate: 44100, burst: 2000, fmt: 0, volCtl: false, dither: false, autoLat: true, vol: 0, play: true, ctl: 0, ip: m.ip || '', room: '', group: [] } })); sinkExtras(); return { ok: true } }
    case 'sonosPick': case 'groupAdd': { const n = node(); if (!n || n.kind !== 'stream' || !['192.168.0.21', '192.168.0.22'].includes(m.ip)) return bad('Zone nicht gefunden – erst suchen'); if (m.op === 'sonosPick') { n.p.ip = m.ip; return { ok: true } } if (m.ip === n.p.ip) return bad('Das ist schon die Haupt-Zone (Koordinator)'); if (!n.p.group.some(g => g.ip === m.ip)) n.p.group.push({ ip: m.ip, name: m.ip }); return { ok: true } }
    case 'groupClear': { const n = node(); if (!n || n.kind !== 'stream') return bad('Ungueltig'); n.p.group = []; return { ok: true } }
    case 'setProp': { const n = node(); if (!n || typeof m.key !== 'string') return bad('Ungueltig'); const v = m.value, isI = Number.isInteger(v), p = n.p || {};
      if (m.key === 'excl') { if (n.kind !== 'out' && n.kind !== 'cap' || typeof v !== 'boolean') return bad('Nur fuer Geraete-Ein- und -Ausgaenge'); p.excl = v; p.mode = v ? 'excl' : 'std'; return { ok: true } }
      if (m.key === 'exRate') { if (n.kind !== 'out' && n.kind !== 'cap' || !isI || !(v === 0 || REC_RATES.includes(v))) return bad('Ungueltig'); p.exRate = v; return { ok: true } }
      if (m.key === 'inPair') { if (n.kind !== 'cap' || !isI || v < 0 || v > 7) return bad('Kanalpaar 1–8 nur bei Eingaengen'); p.pair = v; return { ok: true } }
      if (m.key === 'liveDirect') { if (n.kind !== 'out' && n.kind !== 'vout' || typeof v !== 'boolean') return bad('Ungueltig'); p.liveDirect = v; return { ok: true } }
      if (m.key === 'vbIdx') { if (n.kind !== 'vin' && n.kind !== 'vout' || !isI || !((v >= 0 && v < 8) || (v >= 100 && v < 108 && n.kind === 'vin'))) return bad('Ungueltig'); p.idx = v; n.name = vname(n.kind, v); return { ok: true } }
      if (n.kind === 'rec' && m.key !== 'dir') { const al = p.al, num = typeof v === 'number' ? v : typeof v === 'boolean' ? +v : NaN, ii = Number.isInteger(num); const live = ['alMode', 'alTarget', 'alBoost', 'alCeil', 'alSpeed', 'alReset'].includes(m.key);
        if (n.rec.on && !live) return bad('Waehrend der Aufnahme nicht aenderbar'); if (Number.isNaN(num) && m.key !== 'alReset') return bad('Ungueltig');
        switch (m.key) {
          case 'fmt': if (!ii || num < 0 || num > 5) return bad('Ungueltig'); if ((num === 1 || num === 2) && p.mt) return bad('FLAC kennt nur Stereo – erst Mehrspur ausschalten'); p.fmt = num; break;
          case 'rate': if (!ii || !REC_RATES.includes(num)) return bad('Ungueltige Samplerate'); p.rate = num; break;
          case 'dither': if (!ii || num < -1 || num > 1) return bad('Ungueltig'); p.dither = num; break;
          case 'multitrack': if (!ii || num < 0 || num > 1) return bad('Ungueltig'); if (num && (p.fmt === 1 || p.fmt === 2)) return bad('Mehrspur braucht WAV (FLAC kennt nur Stereo)'); if (num && p.bp) return bad('Bit-perfekt gilt nur fuer eine Stereo-Quelle'); p.mt = num; break;
          case 'bitperfect': if (!ii || num < 0 || num > 1) return bad('Ungueltig'); if (num && p.mt) return bad('Bit-perfekt gilt nur fuer eine Stereo-Quelle – erst Mehrspur ausschalten'); p.bp = !!num; break;
          case 'preRoll': if (!ii || ![0, 2, 5, 10, 30].includes(num)) return bad('Vorlauf: 0, 2, 5, 10 oder 30 s'); p.pre = num; break;
          case 'splitMin': if (!ii || ![0, 10, 30, 60, 120].includes(num)) return bad('Teilung: aus, 10, 30, 60 oder 120 min'); p.split = num; break;
          case 'alMode': if (!ii || num < 0 || num > 2) return bad('Ungueltig'); al.mode = num; break;
          case 'alTarget': if (!ii || num < -30 || num > -10) return bad('Ziel −30 … −10 LUFS'); al.target = num; break;
          case 'alBoost': if (!ii || num < 0 || num > 36) return bad('Anhebung 0 … 36 dB'); al.boost = num; break;
          case 'alCeil': if (num < -12 || num > 0 || Math.abs(num * 2 - Math.round(num * 2)) > 1e-9) return bad('Decke −12 … 0 dBTP (0,5er-Schritte)'); al.ceil = num; break;
          case 'alSpeed': if (!ii || num < 0 || num > 2) return bad('Ungueltig'); al.speed = num; break;
          case 'alReset': n.rec.tp = -120; n.rec.clips = 0; break;
          default: return bad('Einstellung unbekannt') } return { ok: true } }
      if (m.key === 'ext') { if (!n.lat || !num(v, 0, 10000)) return bad('Ungueltig'); n.ext = v; return { ok: true } }
      if (n.kind === 'gen' && m.key === 'gen') { if (!isI || v < 0 || v > 2) return bad('Ungueltig'); p.gen = v; n.p = p; return { ok: true } }
      if (n.kind === 'rec') { if (n.rec.on && m.key !== 'dir') return bad('Waehrend der Aufnahme nicht aenderbar'); if (m.key === 'fmt' && isI && v >= 0 && v <= 4) { p.fmt = v; return { ok: true } } if (m.key === 'rate' && [44100, 48000, 88200, 96000].includes(v)) { p.rate = v; return { ok: true } } if (m.key === 'dir' && typeof v === 'string' && fs.existsSync(v) && fs.statSync(v).isDirectory()) { p.dir = v; return { ok: true } } return bad('Ungueltig') }
      if (n.kind === 'stream') { const ok = { ip: typeof v === 'string' && /^(\d{1,3}\.){3}\d{1,3}$/.test(v) && v.split('.').every(x => +x < 256), port: isI && v >= 1024 && v <= 65535, scheme: isI && (v === 0 || v === 1), rate: v === 44100 || v === 48000, burst: isI && v >= 200 && v <= 8000 && v % 100 === 0, fmt: isI && (v === 0 || v === 1), volCtl: typeof v === 'boolean', dither: typeof v === 'boolean', autoLat: typeof v === 'boolean', vol: isI && v >= 0 && v <= 100 && p.volCtl, play: typeof v === 'boolean' };
        if (!(m.key in ok) || !ok[m.key]) return bad(m.key === 'vol' ? 'Lautstaerke-Steuerung ist aus' : 'Ungueltig'); p[m.key] = v; return { ok: true } }
      if (n.kind === 'air') { const ok = { name: typeof v === 'string' && v.length >= 1 && v.length <= 40, port: isI && v >= 1024 && v <= 65535, buf: isI && v >= 50 && v <= 2000, latAuto: typeof v === 'boolean', latMs: isI && v >= 0 && v <= 4000, useVol: typeof v === 'boolean' }; if (!(m.key in ok) || !ok[m.key]) return bad('Ungueltig'); p[m.key] = v; return { ok: true } }
      return bad('Einstellung unbekannt') }
    case 'revoke': { const d = M.devices.get(m.device); if (!d) return bad('Geraet fehlt'); sessions.delete(d.tok); M.devices.delete(d.id); for (const k of clients) if (k.sess.id === d.id) k.socket.destroy(); return { ok: true } }
    default: return bad('Unbekannter Befehl');
  }
}
function onMsg(c, txt) {
  let m; try { m = JSON.parse(txt) } catch { return } if (!m || m.t !== 'cmd' || !Number.isInteger(m.id)) return;
  const now = Date.now(); c.win = (c.win || []).filter(t => now - t < 1000); if (c.win.length > 60) return sendWs(c, { t: 'ack', id: m.id, ok: false, err: 'zu schnell' }); c.win.push(now);
  const r = exec(c, m); sendWs(c, { t: 'ack', id: m.id, ok: r.ok, err: r.err || '' }); if (r.ok) broadcastState();
}
setInterval(() => {                                                    // Pegel (im Router: aus den VU-Werten der Nodes), 20 Hz
  if (!clients.size) return; const t = Date.now() / 1000, a = [];
  for (const n of M.nodes) { const live = hasOut(n) && n.kind !== 'sum' ? true : M.conns.some(c => c.dst === n.id); const base = live && !n.mute ? -8 + 5 * Math.sin(t * 7 + n.id) - 3 * Math.abs(Math.sin(t * 2.1 + n.id)) + n.gain : -90; a.push([n.id, +base.toFixed(1), +(base - 1.2).toFixed(1), 0]) }
  for (const c of clients) sendWs(c, { t: 'm', a });
}, 50).unref();
setInterval(() => { let ch = false; for (const n of M.nodes) if (n.rec?.on) { n.rec.sec = (Date.now() - n.rec.t0) / 1000; n.rec.bytes = n.rec.sec * 345000; n.status = '● ' + Math.floor(n.rec.sec) + ' s'; ch = true } if (ch) broadcastState() }, 1000).unref();

// ---------------------------------------------------------------- Start
let CERT_PEM, server;
function makeCert() { const d = fs.mkdtempSync(path.join(os.tmpdir(), 'bar-')), k = path.join(d, 'k.pem'), c = path.join(d, 'c.pem');
  cp.execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', k, '-out', c, '-days', '30', '-subj', '/CN=bens-audio-router.local', '-addext', 'subjectAltName=DNS:bens-audio-router.local,DNS:localhost,IP:127.0.0.1'], { stdio: 'ignore' });
  return { key: fs.readFileSync(k), cert: fs.readFileSync(c) } }
function start() { const cr = makeCert(); CERT_PEM = cr.cert; server = https.createServer({ ...cr, minVersion: 'TLSv1.2' }, (q, s) => handler(q, s).catch(() => json(q, s, 500, { err: 'Fehler' })));
  server.on('upgrade', upgrade); return new Promise(r => server.listen(PORT, opt('host', '127.0.0.1'), () => { newCode(); r(server) })) }
module.exports = { start, newCode, M, sessions, resetFails() { fails.clear() }, get server() { return server }, setCodeExpired() { if (pair) pair.exp = 0 } };
if (require.main === module) start().then(s => console.log(`Mock laeuft: https://${opt('host', '127.0.0.1')}:${s.address().port}/  – Kopplungscode: ${pair.code} (gueltig ${CODE_TTL / 1000} s)`));
