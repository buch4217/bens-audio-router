const { test } = require('node:test'), assert = require('node:assert/strict'), fs = require('fs'), path = require('path');
const html = fs.readFileSync(path.join(__dirname, '..', 'index.html'), 'utf8'), js = html.match(/<script>([\s\S]*)<\/script>/)[1];
const m = { exports: {} }; new Function('module', js)(m); const L = m.exports;
const st = () => ({ nodes: [
  { id: 1, kind: 'cap', name: 'A', x: 0, y: 0, ports: 'out' }, { id: 2, kind: 'cap', name: 'B', x: 0, y: 150, ports: 'out' }, { id: 3, kind: 'sum', name: 'S', x: 400, y: 100, ports: 'both' },
  { id: 4, kind: 'out', name: 'O', x: 800, y: 0, ports: 'in' }, { id: 5, kind: 'sum', name: 'S2', x: 400, y: 300, ports: 'both' }], conns: [{ id: 1, src: 1, dst: 3, srcCh: 3, dstCh: 1 }] });
test('Fader: dB <-> Position, Raster bei 0 dB', () => { assert.equal(L.posToDb(L.dbToPos(0)), 0); assert.equal(L.posToDb(0), -60); assert.equal(L.posToDb(1), 0); assert.equal(L.posToDb(.99), 0, 'Raster nahe 0 dB'); assert.equal(L.dbToPos(12), 1, 'nie ueber 0 dB'); assert.ok(Math.abs(L.posToDb(L.dbToPos(-12.3)) - -12.3) < .11); });
test('Meter 0..1 mit Begrenzung', () => { assert.equal(L.meterPos(-90), 0); assert.equal(L.meterPos(0), 1); assert.equal(L.meterPos(6), 1); assert.ok(Math.abs(L.meterPos(-30) - .5) < 1e-9); });
test('Zeit-Format', () => { assert.equal(L.fmtTime(7), '00:07'); assert.equal(L.fmtTime(125), '02:05'); assert.equal(L.fmtTime(3725), '01:02:05'); assert.equal(L.fmtDb(-3), '-3.0 dB'); assert.equal(L.fmtDb(0), '+0.0 dB'); });
test('Matrix: Zeilen = Quellen, Spalten = Senken, Summe in beiden', () => {
  const mm = L.matrixModel(st()); assert.deepEqual(mm.rows.map(n => n.id), [1, 2, 3, 5]); assert.deepEqual(mm.cols.map(n => n.id), [3, 4, 5]);
  assert.ok(mm.cells['1>3'].conn); assert.equal(mm.cells['1>4'].enabled, true); assert.equal(mm.cells['3>5'].enabled, false, 'Summe -> Summe gesperrt'); assert.equal(mm.cells['5>3'].enabled, false); assert.equal(mm.cells['3>4'].enabled, true); });
test('Verbindungsregeln', () => { const s = st(); assert.equal(L.canConnect(s, 1, 3).reason, 'Schon verbunden'); assert.equal(L.canConnect(s, 4, 1).ok, false); assert.equal(L.canConnect(s, 1, 2).ok, false); assert.equal(L.canConnect(s, 3, 3).ok, false);
  assert.match(L.canConnect(s, 3, 5).reason, /hintereinander/); assert.equal(L.canConnect(s, 2, 4).ok, true); assert.equal(L.canConnect(s, 9, 4).ok, false); });
test('Ports und Kabel-Geometrie', () => { const s = st(); assert.deepEqual(L.portPos(s.nodes[0], 'out'), { x: 270, y: 12 }); assert.deepEqual(L.portPos(s.nodes[3], 'in'), { x: 800, y: 12 });
  const h = L.hitPort(s.nodes, { x: 272, y: 14 }, 20); assert.equal(h.node.id, 1); assert.equal(h.side, 'out'); assert.equal(L.hitPort(s.nodes, { x: 600, y: 600 }, 20), null);
  const bz = L.bezierPts({ x: 0, y: 0 }, { x: 400, y: 100 }); const mid = L.bezierPoint(bz, .5); assert.ok(Math.abs(mid.x - 200) < 1 && Math.abs(mid.y - 50) < 1);
  const c = L.hitCable(s, { x: (270 + 400) / 2, y: 12 + (112 - 12) / 2 }, 12); assert.ok(c && c.id === 1); assert.equal(L.hitCable(s, { x: 1000, y: 1000 }, 12), null); });
test('Zoom um Fingerpunkt haelt den Weltpunkt fest, Grenzen', () => { let v = { x: 30, y: -20, k: 1 }; const w0 = L.toWorld(v, 200, 300); v = L.zoomAt(v, 200, 300, 1.7); const w1 = L.toWorld(v, 200, 300); assert.ok(Math.abs(w0.x - w1.x) < 1e-9 && Math.abs(w0.y - w1.y) < 1e-9);
  assert.equal(L.zoomAt({ x: 0, y: 0, k: 2.4 }, 0, 0, 10).k, 2.5); assert.equal(L.zoomAt({ x: 0, y: 0, k: .13 }, 0, 0, .1).k, .12); });
test('Alles zeigen passt in den Rahmen', () => { const s = st(), v = L.fitView(s.nodes, 390, 600); for (const n of s.nodes) { const x = n.x * v.k + v.x, y = n.y * v.k + v.y, x2 = (n.x + 270) * v.k + v.x, y2 = (n.y + L.nodeH(n)) * v.k + v.y; assert.ok(x >= -.5 && y >= -.5 && x2 <= 390.5 && y2 <= 600.5, n.name + ' ausserhalb'); } assert.deepEqual(L.fitView([], 100, 100), { x: 0, y: 0, k: 1 }); });
test('Knotenhoehe mit Sonos-Zonen', () => { assert.equal(L.nodeH({}), 124); assert.equal(L.nodeH({ zones: [1, 2, 3] }), 124 + 24 + 138 + 6); });
