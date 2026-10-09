// PCB2 现状：哪些在板框外？电源区各件距离如何？
var out = { outside: [], pairs: [], count: 0 };
try { await eda.dmt_EditorControl.openDocument('07ed4147375824d3'); } catch (e) { }
function desOf(c) { try { return c.getState_Designator ? (c.getState_Designator() || '') : ''; } catch (e) { return ''; } }
var comps = (await eda.pcb_PrimitiveComponent.getAll()) || [];
out.count = comps.length;
var pos = {};
for (var i = 0; i < comps.length; i++) { var d = desOf(comps[i]); if (!d) continue; pos[d] = { x: comps[i].x, y: comps[i].y }; }
// 板框（实测）x -236.22 ~ 3700.79, y -2319.20 ~ 43
for (var k in pos) {
    var p = pos[k];
    if (p.x < -236 || p.x > 3701 || p.y < -2319 || p.y > 43) out.outside.push({ des: k, x: Math.round(p.x), y: Math.round(p.y) });
}
function distMm(a, b) {
    if (!pos[a] || !pos[b]) return null;
    return Math.round(Math.sqrt(Math.pow(pos[a].x - pos[b].x, 2) + Math.pow(pos[a].y - pos[b].y, 2)) * 0.0254 * 10) / 10;
}
var P = [['C3', 'U2'], ['C1', 'U2'], ['D1', 'U2'], ['L1', 'U2'], ['C2', 'L1'], ['C4', 'C2'], ['C5', 'U3'], ['C6', 'U3'], ['C2', 'U1'], ['C1', 'C13']];
out.pairs = P.map(function (z) { return { a: z[0], b: z[1], mm: distMm(z[0], z[1]) }; });
out.powerPos = {};
['U2', 'L1', 'D1', 'C1', 'C2', 'C3', 'C4', 'U3', 'C5', 'C6'].forEach(function (z) {
    if (pos[z]) out.powerPos[z] = Math.round(pos[z].x) + ',' + Math.round(pos[z].y);
});
return JSON.stringify(out, null, 1);
