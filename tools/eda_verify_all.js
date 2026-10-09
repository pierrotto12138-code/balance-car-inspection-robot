// 完整核验：8 个座 + 新增 R/C/J18 的逐脚网络
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
function desOf(c) { try { return c.getState_Designator ? (c.getState_Designator() || '') : ''; } catch (e) { return ''; } }
function devOf(c) { try { var d = c.getState_Component ? c.getState_Component() : null; return (d && d.name) || ''; } catch (e) { return ''; } }
function netOfFlag(c) { try { return c.net || ''; } catch (e) { return ''; } }

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

// 电源网名 → 显示名
function powerLabel(c) {
    var n = netOfFlag(c);
    if (n.indexOf('POWER_') === 0) return n.replace('POWER_', '');
    var d = devOf(c);
    if (d.indexOf('power_') === 0) return d.replace('power_', '');
    return null;
}

function resolve(x, y) {
    var seen = {}, q = [K(x, y)], nets = {}, pw = {};
    while (q.length) {
        var cur = q.shift();
        if (seen[cur]) continue;
        seen[cur] = 1;
        var cs = compAt[cur] || [];
        for (var a = 0; a < cs.length; a++) {
            var lab = powerLabel(cs[a]);
            if (lab) pw[lab] = 1;
        }
        var ws = wireAt[cur] || [];
        for (var b = 0; b < ws.length; b++) {
            if (ws[b].net) nets[ws[b].net] = 1;
            var v2 = ptsOf(ws[b].line);
            for (var c2 = 0; c2 < v2.length; c2++) {
                var nk = K(v2[c2][0], v2[c2][1]);
                if (!seen[nk]) q.push(nk);
            }
        }
    }
    return { nets: Object.keys(nets), pw: Object.keys(pw) };
}

var EXPECT = {
    J13: ['MQ2_AO', 'PB3', 'GNDREF', '+5V'], J10: ['MQ135_AO', 'PC3', 'GNDREF', '+5V'],
    J9: ['GNDREF', '+3.3V', 'PA7'], J11: ['+5V', 'GNDREF', 'PB12'], J14: ['+5V', 'GNDREF', 'PC1'],
    J5: ['+5V', 'PC4', 'PB0', 'GNDREF'], J7: ['(空)', 'PA2', 'PA3', 'GNDREF', '+5V', '(空)'],
    R10: ['MQ2_AO', 'PA5'], R11: ['PA5', 'GNDREF'], R12: ['MQ135_AO', 'PA6'], R13: ['PA6', 'GNDREF'],
    R14: ['PA7', '+3.3V'], C14: ['PA5', 'GNDREF'], C15: ['PA6', 'GNDREF'],
    C16: ['+5V', 'GNDREF'], C17: ['+5V', 'GNDREF'], C18: ['+5V', 'GNDREF'], C19: ['+5V', 'GNDREF'],
    J18: ['+5V', 'PC5', 'GNDREF']
};

var res = [], bad = [];
for (var m = 0; m < comps.length; m++) {
    var d = desOf(comps[m]);
    if (!EXPECT[d]) continue;
    var pins = (await comps[m].getAllPins()) || [];
    var rows = [];
    for (var q2 = 0; q2 < pins.length; q2++) {
        var r = resolve(pins[q2].x, pins[q2].y);
        var got = r.pw.concat(r.nets).join('|') || '(无)';
        rows.push({ pin: String(pins[q2].pinNumber), got: got });
    }
    rows.sort(function (a, b) { return (parseInt(a.pin) || 0) - (parseInt(b.pin) || 0); });
    var exp = EXPECT[d], line = [];
    for (var z = 0; z < rows.length; z++) {
        var e = exp[z] !== undefined ? exp[z] : '?';
        var ok = (rows[z].got === e);
        if (!ok) bad.push(d + '.' + rows[z].pin + ' 实测=' + rows[z].got + ' 期望=' + e);
        line.push(rows[z].pin + ':' + rows[z].got + (ok ? '' : '<<<'));
    }
    res.push({ des: d, line: line.join(' | ') });
}
out.result = res;
out.mismatch = bad;
out.totalComps = comps.length;
return JSON.stringify(out, null, 1);
