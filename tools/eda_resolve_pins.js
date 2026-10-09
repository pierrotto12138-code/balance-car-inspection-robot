// 解算出各座"引脚 → 网络"（只读）：导线网名 + 电源符号，按几何连通性走线
var out = { t: [] };
function mark(s) { out.t.push(Math.round(performance.now()) + 'ms ' + s); }

var PAGE = 'c405558037e65ebd';   // 改板 P1
try { await eda.dmt_EditorControl.openDocument(PAGE); } catch (e) { out.openErr = String(e); }

var comps = (await eda.sch_PrimitiveComponent.getAll()) || [];
var wires = (await eda.sch_PrimitiveWire.getAll()) || [];
mark('read comps+wires');

function K(x, y) { return Math.round(x) + ',' + Math.round(y); }
function ptsOf(line) {
    var a = line;
    if (!Array.isArray(a)) return [];
    if (a.length && Array.isArray(a[0])) return a;      // [[x,y],[x,y]]
    var r = [];
    for (var i = 0; i + 1 < a.length; i += 2) r.push([a[i], a[i + 1]]);
    return r;
}

// 点 → 元件（用于识别电源符号）
var compAt = {};
for (var i = 0; i < comps.length; i++) {
    var c = comps[i];
    var des = '';
    try { des = c.getState_Designator ? (c.getState_Designator() || '') : ''; } catch (e) { }
    var dev = '';
    try { var d = c.getState_Component ? c.getState_Component() : null; dev = (d && d.name) || ''; } catch (e) { }
    var k = K(c.x, c.y);
    (compAt[k] = compAt[k] || []).push({ des: des, dev: dev });
}

// 点 → 导线
var wireAt = {};
for (var w = 0; w < wires.length; w++) {
    var wire = wires[w];
    var pts = ptsOf(wire.line);
    for (var p = 0; p < pts.length; p++) {
        var kk = K(pts[p][0], pts[p][1]);
        (wireAt[kk] = wireAt[kk] || []).push(wire);
    }
}

// 从某点出发，沿导线连通域收集：网名 与 电源符号
function resolve(x, y) {
    var start = K(x, y);
    var seen = {}, queue = [start], nets = {}, flags = [];
    while (queue.length) {
        var cur = queue.shift();
        if (seen[cur]) continue;
        seen[cur] = 1;
        var cs = compAt[cur] || [];
        for (var q = 0; q < cs.length; q++) {
            if (cs[q].dev && cs[q].dev.indexOf('power_') === 0) flags.push(cs[q].dev.replace('power_', ''));
        }
        var ws = wireAt[cur] || [];
        for (var r = 0; r < ws.length; r++) {
            var ww = ws[r];
            if (ww.net) nets[ww.net] = 1;
            var pp = ptsOf(ww.line);
            for (var s = 0; s < pp.length; s++) {
                var nk = K(pp[s][0], pp[s][1]);
                if (!seen[nk]) queue.push(nk);
            }
        }
    }
    var netList = Object.keys(nets);
    var flagList = flags.filter(function (v, idx) { return flags.indexOf(v) === idx; });
    return { nets: netList, flags: flagList };
}

// 关心的座
var watch = ['J5', 'J7', 'J9', 'J10', 'J11', 'J13', 'J14', 'J15'];
var result = [];
for (var m = 0; m < comps.length; m++) {
    var cm = comps[m];
    var dm = '';
    try { dm = cm.getState_Designator ? (cm.getState_Designator() || '') : ''; } catch (e) { }
    if (watch.indexOf(dm) < 0) continue;
    var devName = '';
    try { var dd = cm.getState_Component ? cm.getState_Component() : null; devName = (dd && dd.name) || ''; } catch (e) { }
    var pins = [];
    try { pins = (await cm.getAllPins()) || []; } catch (e) { }
    var rows = [];
    for (var n = 0; n < pins.length; n++) {
        var pn = pins[n];
        var res = resolve(pn.x, pn.y);
        rows.push({
            pin: pn.pinNumber,
            x: pn.x, y: pn.y,
            net: res.nets.join('|') || '(无)',
            flag: res.flags.join('|') || ''
        });
    }
    rows.sort(function (a, b) { return (parseInt(a.pin) || 0) - (parseInt(b.pin) || 0); });
    result.push({ des: dm, dev: devName, x: cm.x, y: cm.y, pins: rows });
}
out.sockets = result;
mark('resolved');
return JSON.stringify(out, null, 1);
