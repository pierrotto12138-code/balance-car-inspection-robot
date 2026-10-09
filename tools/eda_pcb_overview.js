// 总览：走线/过孔/铺铜 + 各网络连通 + DRC
var out = { nets: {}, widths: {}, layers: {}, connNets: [] };
try { await eda.dmt_EditorControl.openDocument('07ed4147375824d3'); } catch (e) { out.openErr = String(e); return JSON.stringify(out, null, 1); }
function desOf(c) { try { return c.getState_Designator ? (c.getState_Designator() || '') : ''; } catch (e) { return ''; } }
var comps = (await eda.pcb_PrimitiveComponent.getAll()) || [];
var pads = [];
for (var i = 0; i < comps.length; i++) {
    var d = desOf(comps[i]); if (!d) continue;
    var ps = []; try { ps = (await comps[i].getAllPins()) || []; } catch (e) { }
    for (var p = 0; p < ps.length; p++) pads.push({ des: d, pin: String(ps[p].padNumber), net: String(ps[p].net || ''), x: ps[p].x, y: ps[p].y });
}
var lines = (await eda.pcb_PrimitiveLine.getAll()) || [];
var vias = (await eda.pcb_PrimitiveVia.getAll()) || [];
out.lineCount = lines.length; out.viaCount = vias.length;
for (var j = 0; j < lines.length; j++) {
    var w = String(Math.round(Number(lines[j].lineWidth)));
    out.widths[w] = (out.widths[w] || 0) + 1;
    out.layers[String(lines[j].layer)] = (out.layers[String(lines[j].layer)] || 0) + 1;
}
try { out.pours = ((await eda.pcb_PrimitivePour.getAll()) || []).length; } catch (e) { }
function onSeg(p, net) {
    for (var s = 0; s < lines.length; s++) {
        var L = lines[s];
        if (String(L.net) !== net) continue;
        var x1 = L.startX, y1 = L.startY, x2 = L.endX, y2 = L.endY;
        var dx = x2 - x1, dy = y2 - y1, len2 = dx * dx + dy * dy;
        var t = len2 ? ((p.x - x1) * dx + (p.y - y1) * dy) / len2 : 0;
        t = Math.max(0, Math.min(1, t));
        if (Math.sqrt(Math.pow(x1 + dx * t - p.x, 2) + Math.pow(y1 + dy * t - p.y, 2)) < 4) return true;
    }
    for (var v = 0; v < vias.length; v++) {
        if (String(vias[v].net) !== net) continue;
        if (Math.sqrt(Math.pow(vias[v].x - p.x, 2) + Math.pow(vias[v].y - p.y, 2)) < 20) return true;
    }
    return false;
}
// 按网络汇总
var byNet = {};
for (var m = 0; m < pads.length; m++) {
    var pd = pads[m], n = pd.net; if (!n) continue;
    var r = byNet[n] = byNet[n] || { total: 0, done: 0, undone: [] };
    r.total++;
    if (onSeg(pd, n)) r.done++;
    else r.undone.push(pd.des + '.' + pd.pin);
}
out.nets = byNet;
var fully = [], partial = [], none = [];
for (var k in byNet) {
    if (byNet[k].done === byNet[k].total) fully.push(k);
    else if (byNet[k].done === 0) none.push(k + '(' + byNet[k].total + ')');
    else partial.push(k + ' ' + byNet[k].done + '/' + byNet[k].total);
}
out.fullyDone = fully.length;
out.partial = partial;
out.notStarted = none;
// DRC
try {
    var r = await eda.pcb_Drc.check(true, false, true);
    (function walk(n, dep) {
        if (!n || dep > 5) return;
        if (Array.isArray(n)) { for (var a = 0; a < n.length; a++) walk(n[a], dep + 1); return; }
        var t = n.title !== undefined ? (typeof n.title === 'object' ? String(n.title[0]) : String(n.title)) : null;
        if (t && n.count !== undefined && dep <= 2) out.connNets.push(t + '=' + n.count);
        if (n.list) walk(n.list, dep + 1);
    })(r, 0);
    var s = JSON.stringify(r);
    out.hasNetlistErr = /Netlist Error/i.test(s);
    out.hasClearance = /Clearance|Spacing|Width/i.test(s);
} catch (e) { out.drcErr = String(e); }
var seen = {}, u = [];
for (var q = 0; q < out.connNets.length; q++) { if (!seen[out.connNets[q]]) { seen[out.connNets[q]] = 1; u.push(out.connNets[q]); } }
out.connNets = u;
return JSON.stringify(out, null, 1);
