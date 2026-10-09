// 全局防短路检查：从各电源符号出发 BFS，看电源网之间是否互相串通
var out = {};
try { await eda.dmt_EditorControl.openDocument('c405558037e65ebd'); } catch (e) { out.openErr = String(e); }

function K(x, y) { return Math.round(x) + ',' + Math.round(y); }
function ptsOf(line) {
    var a = line;
    if (!Array.isArray(a)) return [];
    if (a.length && Array.isArray(a[0])) return a;
    var r = [];
    for (var i = 0; i + 1 < a.length; i += 2) r.push([a[i], a[i + 1]]);
    return r;
}
function devOf(c) { try { var d = c.getState_Component ? c.getState_Component() : null; return (d && d.name) || ''; } catch (e) { return ''; } }

var comps = (await eda.sch_PrimitiveComponent.getAll()) || [];
var wires = (await eda.sch_PrimitiveWire.getAll()) || [];

var compAt = {}, wireAt = {};
for (var i = 0; i < comps.length; i++) (compAt[K(comps[i].x, comps[i].y)] = compAt[K(comps[i].x, comps[i].y)] || []).push(comps[i]);
for (var w = 0; w < wires.length; w++) {
    var vp = ptsOf(wires[w].line);
    for (var p = 0; p < vp.length; p++) {
        var kk = K(vp[p][0], vp[p][1]);
        (wireAt[kk] = wireAt[kk] || []).push(wires[w]);
    }
}

function reach(startPt) {
    var seen = {}, q = [startPt], flags = {}, nets = {};
    while (q.length) {
        var cur = q.shift();
        if (seen[cur]) continue;
        seen[cur] = 1;
        var cs = compAt[cur] || [];
        for (var a = 0; a < cs.length; a++) {
            var dv = devOf(cs[a]);
            if (dv.indexOf('power_') === 0) flags[dv] = (flags[dv] || 0) + 1;
        }
        var ws = wireAt[cur] || [];
        for (var b = 0; b < ws.length; b++) {
            if (ws[b].net) nets[ws[b].net] = 1;
            var vp2 = ptsOf(ws[b].line);
            for (var c = 0; c < vp2.length; c++) {
                var nk = K(vp2[c][0], vp2[c][1]);
                if (!seen[nk]) q.push(nk);
            }
        }
    }
    return { flags: Object.keys(flags), nets: Object.keys(nets) };
}

// 找每个电源符号，从它出发看能到哪些电源符号
var res = [];
for (var m = 0; m < comps.length; m++) {
    var dv2 = devOf(comps[m]);
    if (dv2 !== 'power_+5V' && dv2 !== 'power_GNDREF') continue;
    var r = reach(K(comps[m].x, comps[m].y));
    res.push({ from: dv2 + '@' + K(comps[m].x, comps[m].y), reaches: r.flags });
}
// 汇总：+5V 是否触到 GND，GND 是否触到 +5V
var bad = [];
res.forEach(function (x) {
    if (x.from.indexOf('power_+5V') === 0 && x.reaches.indexOf('power_GNDREF') >= 0) bad.push(x.from + ' 触到 GND');
    if (x.from.indexOf('power_GNDREF') === 0 && x.reaches.indexOf('power_+5V') >= 0) bad.push(x.from + ' 触到 +5V');
});
out.checked = res.length;
out.shorts = bad;
out.sample = res.slice(0, 4);
return JSON.stringify(out, null, 1);
