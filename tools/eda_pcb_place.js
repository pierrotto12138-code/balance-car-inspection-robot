// 读 PCB2 全部元件坐标，做"应该近/应该远"的距离核查
var out = { comps: [], stats: null };
try { await eda.dmt_EditorControl.openDocument('07ed4147375824d3'); } catch (e) { }
function desOf(c) { try { return c.getState_Designator ? (c.getState_Designator() || '') : ''; } catch (e) { return ''; } }
function layerOf(c) { try { return c.getState_Layer ? c.getState_Layer() : ''; } catch (e) { return ''; } }

var comps = (await eda.pcb_PrimitiveComponent.getAll()) || [];
var pts = [];
for (var i = 0; i < comps.length; i++) {
    var c = comps[i], d = desOf(c);
    if (!d) continue;
    var o = { des: d, x: Math.round(c.x * 100) / 100, y: Math.round(c.y * 100) / 100, rot: c.rotation };
    try { var f = c.getState_Footprint(); o.fp = (f && f.name) || ''; } catch (e) { }
    try { o.layer = layerOf(c); } catch (e) { }
    pts.push(o);
}
out.comps = pts;
out.count = pts.length;

// 板框范围
var minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
for (var k = 0; k < pts.length; k++) {
    if (pts[k].x < minX) minX = pts[k].x; if (pts[k].x > maxX) maxX = pts[k].x;
    if (pts[k].y < minY) minY = pts[k].y; if (pts[k].y > maxY) maxY = pts[k].y;
}
out.bbox = { minX: minX, maxX: maxX, minY: minY, maxY: maxY, w: Math.round(maxX - minX), h: Math.round(maxY - minY) };

// 关键配对距离
var byName = {};
for (var m = 0; m < pts.length; m++) byName[pts[m].des] = pts[m];
function distOf(a, b) {
    var A = byName[a], B = byName[b];
    if (!A || !B) return null;
    return Math.round(Math.sqrt((A.x - B.x) * (A.x - B.x) + (A.y - B.y) * (A.y - B.y)) * 10) / 10;
}
var PAIRS = [
    ['C14', 'J13'], ['R10', 'J13'], ['R11', 'R10'], ['C16', 'J13'], ['C18', 'J13'],
    ['C15', 'J10'], ['R12', 'J10'], ['R13', 'R12'], ['C17', 'J10'], ['C19', 'J10'],
    ['R14', 'J9'],
    ['C7', 'U1'], ['C10', 'C7'], ['C8', 'U1'],
    ['C1', 'U2'], ['C3', 'U2'], ['D1', 'U2'], ['L1', 'U2'], ['C2', 'L1'], ['C4', 'C2'],
    ['C5', 'U3'], ['C6', 'U3'],
    ['C9', 'CN1'], ['C11', 'CN2'],
    ['C13', 'J17'], ['C13', 'SW2'], ['J17', 'SW2'],
    ['J1', 'J2']
];
out.pairs = PAIRS.map(function (p) { return { a: p[0], b: p[1], dist: distOf(p[0], p[1]) }; });

// 模拟区 vs 电源/电机区的最近距离
var ANALOG = ['J10', 'J13', 'R10', 'R11', 'R12', 'R13', 'C14', 'C15'];
var NOISY = ['U2', 'L1', 'D1', 'C1', 'C2', 'C3', 'C4', 'U1', 'CN1', 'CN2', 'C7', 'C10'];
var cross = [];
for (var a = 0; a < ANALOG.length; a++) for (var b = 0; b < NOISY.length; b++) {
    var dd = distOf(ANALOG[a], NOISY[b]);
    if (dd !== null) cross.push({ a: ANALOG[a], b: NOISY[b], d: dd });
}
cross.sort(function (x, y) { return x.d - y.d; });
out.closestAnalogNoisy = cross.slice(0, 8);
return JSON.stringify(out, null, 1);
