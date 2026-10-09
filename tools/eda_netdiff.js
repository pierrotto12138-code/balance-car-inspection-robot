// 精确比对两份网表（PCB 侧用 padNumber）
var out = { diff: [], schCount: 0, pcbCount: 0, samples: [] };
function K(x, y) { return Math.round(x) + ',' + Math.round(y); }
function desOf(c) { try { return c.getState_Designator ? (c.getState_Designator() || '') : ''; } catch (e) { return ''; } }
function ptsOf(line) {
    var a = line;
    if (!Array.isArray(a)) return [];
    if (a.length && Array.isArray(a[0])) return a;
    var r = [];
    for (var i = 0; i + 1 < a.length; i += 2) r.push([a[i], a[i + 1]]);
    return r;
}
var schNets = {}, pcbNets = {};
// --- 原理图 ---
try {
    await eda.dmt_EditorControl.openDocument('c405558037e65ebd');
    var comps = (await eda.sch_PrimitiveComponent.getAll()) || [];
    var wires = (await eda.sch_PrimitiveWire.getAll()) || [];
    var wireAt = {}, flagAt = {};
    for (var w = 0; w < wires.length; w++) {
        var vp = ptsOf(wires[w].line);
        for (var p = 0; p < vp.length; p++) { var kk = K(vp[p][0], vp[p][1]); (wireAt[kk] = wireAt[kk] || []).push(wires[w]); }
    }
    for (var i = 0; i < comps.length; i++) { try { var n = comps[i].net || ''; if (n.indexOf('POWER_') === 0) flagAt[K(comps[i].x, comps[i].y)] = n; } catch (e) { } }
    function netName(x, y) {
        var seen = {}, q = [K(x, y)], nets = {}, pw = {};
        while (q.length) {
            var cur = q.shift(); if (seen[cur]) continue; seen[cur] = 1;
            if (flagAt[cur]) pw[flagAt[cur]] = 1;
            var ws = wireAt[cur] || [];
            for (var a = 0; a < ws.length; a++) {
                if (ws[a].net) nets[ws[a].net] = 1;
                var v2 = ptsOf(ws[a].line);
                for (var b = 0; b < v2.length; b++) { var nk = K(v2[b][0], v2[b][1]); if (!seen[nk]) q.push(nk); }
            }
        }
        var arr = Object.keys(pw).concat(Object.keys(nets)).filter(function (z) { return z.indexOf('#PWR') !== 0; });
        return arr.length ? arr.sort().join('|') : null;
    }
    for (var m = 0; m < comps.length; m++) {
        var d = desOf(comps[m]);
        if (!d || d.indexOf('#PWR') === 0) continue;
        var ps = [];
        try { ps = (await comps[m].getAllPins()) || []; } catch (e) { }
        for (var q2 = 0; q2 < ps.length; q2++) {
            var nm = netName(ps[q2].x, ps[q2].y);
            if (!nm) continue;
            (schNets[nm] = schNets[nm] || []).push(d + '.' + String(ps[q2].pinNumber));
        }
    }
    out.schCount = Object.keys(schNets).length;
} catch (e) { out.schErr = String(e); }
// --- PCB ---
try {
    await eda.dmt_EditorControl.openDocument('07ed4147375824d3');
    var pc = (await eda.pcb_PrimitiveComponent.getAll()) || [];
    for (var c2 = 0; c2 < pc.length; c2++) {
        var dc = desOf(pc[c2]);
        if (!dc) continue;
        var p2 = [];
        try { p2 = (await pc[c2].getAllPins()) || []; } catch (e) { }
        for (var q3 = 0; q3 < p2.length; q3++) {
            var nm2 = String(p2[q3].net || '');
            if (!nm2) continue;
            (pcbNets[nm2] = pcbNets[nm2] || []).push(dc + '.' + String(p2[q3].padNumber));
        }
    }
    out.pcbCount = Object.keys(pcbNets).length;
} catch (e) { out.pcbErr = String(e); }
// --- 比对 ---
function norm(a) { return (a || []).slice().sort().join(','); }
var all = {};
for (var a1 in schNets) all[a1] = 1;
for (var a2 in pcbNets) all[a2] = 1;
for (var nm3 in all) {
    var s = norm(schNets[nm3]), p = norm(pcbNets[nm3]);
    if (s !== p) out.diff.push({ net: nm3, sch: s || '(无)', pcb: p || '(无)' });
}
out.diffCount = out.diff.length;
// 只报"成员不同"的（忽略两边都空/自动命名）
out.realDiff = out.diff.filter(function (z) { return z.sch !== '(无)' && z.pcb !== '(无)'; });
return JSON.stringify(out, null, 1);
