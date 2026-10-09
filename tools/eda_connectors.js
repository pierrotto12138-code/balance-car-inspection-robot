// 读全部接口（座/连接器）的逐脚网络，并追踪每个网络连到哪里
var out = { conns: [], total: 0 };
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
function desOf(c) { try { return c.getState_Designator ? (c.getState_Designator() || '') : ''; } catch (e) { return ''; } }
function devOf(c) { try { var d = c.getState_Component ? c.getState_Component() : null; return (d && d.name) || ''; } catch (e) { return ''; } }
function netOfFlag(c) { try { var n = c.net || ''; return n.indexOf('POWER_') === 0 ? n : null; } catch (e) { return null; } }

var comps = (await eda.sch_PrimitiveComponent.getAll()) || [];
var wires = (await eda.sch_PrimitiveWire.getAll()) || [];
out.total = comps.length;

var wireAt = {}, flagAt = {}, pinAt = {};
for (var w = 0; w < wires.length; w++) {
    var vp = ptsOf(wires[w].line);
    for (var p = 0; p < vp.length; p++) { var kk = K(vp[p][0], vp[p][1]); (wireAt[kk] = wireAt[kk] || []).push(wires[w]); }
}
for (var i = 0; i < comps.length; i++) {
    var f = netOfFlag(comps[i]);
    if (f) flagAt[K(comps[i].x, comps[i].y)] = f;
}
// 所有元件的引脚点 -> 名字
for (var m = 0; m < comps.length; m++) {
    var d = desOf(comps[m]);
    if (!d) continue;
    var ps = (await comps[m].getAllPins()) || [];
    for (var q = 0; q < ps.length; q++) {
        var key = K(ps[q].x, ps[q].y);
        (pinAt[key] = pinAt[key] || []).push(d + '.' + String(ps[q].pinNumber) + (ps[q].pinName ? '/' + ps[q].pinName : ''));
    }
}
function trace(x, y) {
    var seen = {}, q = [K(x, y)], nets = {}, pw = {}, touch = {};
    while (q.length) {
        var cur = q.shift();
        if (seen[cur]) continue; seen[cur] = 1;
        if (flagAt[cur]) pw[flagAt[cur].replace('POWER_', '')] = 1;
        (pinAt[cur] || []).forEach(function (t) { touch[t] = 1; });
        var ws = wireAt[cur] || [];
        for (var a = 0; a < ws.length; a++) {
            if (ws[a].net) nets[ws[a].net] = 1;
            var v2 = ptsOf(ws[a].line);
            for (var b = 0; b < v2.length; b++) { var nk = K(v2[b][0], v2[b][1]); if (!seen[nk]) q.push(nk); }
        }
    }
    return { label: Object.keys(pw).concat(Object.keys(nets)).join('|') || '(无名)', pins: Object.keys(touch).sort() };
}

// 找出所有"连接器类"元件：位号 J*/CN*
for (var z = 0; z < comps.length; z++) {
    var dz = desOf(comps[z]);
    if (!dz || dz.indexOf('#PWR') === 0) continue;
    if (!/^(J|CN)\d+$/.test(dz)) continue;
    var cz = comps[z], o = { des: dz, dev: devOf(cz), x: cz.x, y: cz.y, pins: [] };
    var pz = (await cz.getAllPins()) || [];
    for (var v = 0; v < pz.length; v++) {
        var tr = trace(pz[v].x, pz[v].y);
        o.pins.push({
            n: String(pz[v].pinNumber), name: pz[v].pinName || '',
            label: tr.label,
            dest: tr.pins.filter(function (t) { return !t.startsWith(dz + '.'); })
        });
    }
    out.conns.push(o);
}
out.connCount = out.conns.length;
return JSON.stringify(out, null, 1);
