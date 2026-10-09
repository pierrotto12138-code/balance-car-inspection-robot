// 修复（写入已恢复）：
//  1) 清探测残留  2) 修 C18/C19 短路  3) 重建插座区 J13/J10/J11/J14/J18
var out = { log: [] };
try { await eda.dmt_EditorControl.openDocument('c405558037e65ebd'); } catch (e) { out.openErr = String(e); }
function L(o) { out.log.push(o); }
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
function flagNet(c) { try { var n = c.net || ''; return n.indexOf('POWER_') === 0 ? n : null; } catch (e) { return null; } }
function short(net) { return net.replace('POWER_', ''); }

// ---------- 1) 清探测残留（1900 附近） ----------
var w0 = (await eda.sch_PrimitiveWire.getAll()) || [];
var kill0 = [];
for (var i = 0; i < w0.length; i++) {
    var vp = ptsOf(w0[i].line);
    if (!vp.length) continue;
    var ins = true;
    for (var v = 0; v < vp.length; v++) if (!(vp[v][0] >= 1850 && vp[v][1] >= 1850)) { ins = false; break; }
    if (ins) kill0.push(w0[i].primitiveId);
}
if (kill0.length) { try { await eda.sch_PrimitiveWire.delete(kill0); L({ clearedTestWires: kill0.length }); } catch (e) { L({ testWireErr: String(e) }); } }
var c0 = (await eda.sch_PrimitiveComponent.getAll()) || [];
var killF0 = [];
for (var j = 0; j < c0.length; j++) if (c0[j].x >= 1850 && c0[j].y >= 1850) killF0.push(c0[j].primitiveId);
if (killF0.length) { try { await eda.sch_PrimitiveComponent.delete(killF0); L({ clearedTestFlags: killF0.length }); } catch (e) { L({ testFlagErr: String(e) }); } }

// ---------- 2) 修 C18/C19：pin1 左接 +5V，pin2 右接 GND ----------
async function fixCap(des) {
    var comps = (await eda.sch_PrimitiveComponent.getAll()) || [];
    var me = null;
    for (var i = 0; i < comps.length; i++) if (desOf(comps[i]) === des) { me = comps[i]; break; }
    if (!me) { L({ des: des, err: 'not found' }); return; }
    var pins = (await me.getAllPins()) || [];
    var byNum = {};
    for (var p = 0; p < pins.length; p++) byNum[String(pins[p].pinNumber)] = { x: pins[p].x, y: pins[p].y };
    var p1 = byNum['1'], p2 = byNum['2'];
    if (!p1 || !p2) { L({ des: des, err: 'pins missing' }); return; }

    var bx0 = Math.min(p1.x, p2.x) - 60, bx1 = Math.max(p1.x, p2.x) + 60;
    var by0 = Math.min(p1.y, p2.y) - 50, by1 = Math.max(p1.y, p2.y) + 50;

    // 删该范围内的导线
    var ws = (await eda.sch_PrimitiveWire.getAll()) || [];
    var dw = [];
    for (var w = 0; w < ws.length; w++) {
        var vv = ptsOf(ws[w].line);
        if (!vv.length) continue;
        var ins2 = true;
        for (var q = 0; q < vv.length; q++) if (!(vv[q][0] >= bx0 && vv[q][0] <= bx1 && vv[q][1] >= by0 && vv[q][1] <= by1)) { ins2 = false; break; }
        if (ins2) dw.push(ws[w].primitiveId);
    }
    if (dw.length) { try { await eda.sch_PrimitiveWire.delete(dw); L({ des: des, delWires: dw.length }); } catch (e) { L({ des: des, dwErr: String(e) }); } }

    // 删该范围内的电源符号
    var cs = (await eda.sch_PrimitiveComponent.getAll()) || [];
    var df = [];
    for (var k = 0; k < cs.length; k++) {
        if (!flagNet(cs[k])) continue;
        if (cs[k].x >= bx0 && cs[k].x <= bx1 && cs[k].y >= by0 && cs[k].y <= by1) df.push(cs[k].primitiveId);
    }
    if (df.length) { try { await eda.sch_PrimitiveComponent.delete(df); L({ des: des, delFlags: df.length }); } catch (e) { L({ des: des, dfErr: String(e) }); } }

    // 重建：pin1 向左 → +5V；pin2 向右 → GND
    var f1x = p1.x - 45, f2x = p2.x + 45;
    try {
        await eda.sch_PrimitiveWire.create([p1.x, p1.y, f1x, p1.y], '');
        var fa = await eda.sch_PrimitiveComponent.createNetFlag('Power', 'POWER_+5V', f1x, p1.y);
        L({ des: des, pin1: '+5V', at: K(f1x, p1.y), net: fa && fa.net });
    } catch (e) { L({ des: des, p1Err: String(e) }); }
    try {
        await eda.sch_PrimitiveWire.create([p2.x, p2.y, f2x, p2.y], '');
        var fb = await eda.sch_PrimitiveComponent.createNetFlag('Ground', 'POWER_GNDREF', f2x, p2.y);
        L({ des: des, pin2: 'GND', at: K(f2x, p2.y), net: fb && fb.net });
    } catch (e) { L({ des: des, p2Err: String(e) }); }
}
await fixCap('C18');
await fixCap('C19');

// ---------- 3) 重建插座区 ----------
var RX0 = 1000, RX1 = 1165, RY0 = 190, RY1 = 470;
var ws2 = (await eda.sch_PrimitiveWire.getAll()) || [];
var kc = [];
for (var m = 0; m < ws2.length; m++) {
    var vp2 = ptsOf(ws2[m].line);
    if (!vp2.length) continue;
    var ins3 = true;
    for (var n = 0; n < vp2.length; n++) if (!(vp2[n][0] >= RX0 && vp2[n][0] <= RX1 && vp2[n][1] >= RY0 && vp2[n][1] <= RY1)) { ins3 = false; break; }
    if (ins3) kc.push(ws2[m].primitiveId);
}
if (kc.length) { try { L({ clearedCluster: kc.length, ok: await eda.sch_PrimitiveWire.delete(kc) }); } catch (e) { L({ kcErr: String(e) }); } }

var comps3 = (await eda.sch_PrimitiveComponent.getAll()) || [];
async function pinMap(des) {
    for (var i = 0; i < comps3.length; i++) {
        if (desOf(comps3[i]) === des) {
            var ps = (await comps3[i].getAllPins()) || [];
            var mp = {};
            for (var j = 0; j < ps.length; j++) mp[String(ps[j].pinNumber)] = { x: ps[j].x, y: ps[j].y };
            return mp;
        }
    }
    return null;
}
var flags = [];
for (var f = 0; f < comps3.length; f++) {
    var nn = flagNet(comps3[f]);
    if (nn) flags.push({ net: nn, x: comps3[f].x, y: comps3[f].y });
}
function nearest(net, x, y) {
    var best = null, bd = 1e9;
    for (var i = 0; i < flags.length; i++) {
        if (flags[i].net !== net) continue;
        var d = Math.abs(flags[i].x - x) + Math.abs(flags[i].y - y);
        if (d < bd) { bd = d; best = flags[i]; }
    }
    return best;
}

var T = {
    J13: { '1': { s: 'MQ2_AO' }, '2': { s: 'PB3' }, '3': { p: 'POWER_GNDREF' }, '4': { p: 'POWER_+5V' } },
    J10: { '1': { s: 'MQ135_AO' }, '2': { s: 'PC3' }, '3': { p: 'POWER_GNDREF' }, '4': { p: 'POWER_+5V' } },
    J11: { '1': { p: 'POWER_+5V' }, '2': { p: 'POWER_GNDREF' }, '3': { s: 'PB12' } },
    J14: { '1': { p: 'POWER_+5V' }, '2': { p: 'POWER_GNDREF' }, '3': { s: 'PC1' } },
    J18: { '1': { p: 'POWER_+5V' }, '2': { s: 'PC5' }, '3': { p: 'POWER_GNDREF' } }
};
var laneSeq = 0;
for (var des2 in T) {
    var pm = await pinMap(des2);
    if (!pm) { L({ des: des2, err: 'pinmap null' }); continue; }
    for (var pk in T[des2]) {
        var t = T[des2][pk], pp = pm[pk];
        if (!pp) { L({ des: des2, pin: pk, err: 'no pin' }); continue; }
        if (t.s) {
            try { await eda.sch_PrimitiveWire.create([pp.x, pp.y, pp.x - 25, pp.y], t.s); L({ des: des2, pin: pk, sig: t.s }); }
            catch (e) { L({ des: des2, pin: pk, sigErr: String(e) }); }
        } else {
            var fl = nearest(t.p, pp.x, pp.y);
            if (!fl) {
                var kind = (t.p === 'POWER_GNDREF') ? 'Ground' : 'Power';
                var fx = pp.x - 45, fy = pp.y;
                try {
                    var nf = await eda.sch_PrimitiveComponent.createNetFlag(kind, t.p, fx, fy);
                    fl = { net: t.p, x: fx, y: fy }; flags.push(fl);
                    L({ des: des2, pin: pk, newFlag: short(t.p), at: K(fx, fy), ok: !!nf });
                } catch (e) { L({ des: des2, pin: pk, flagErr: String(e) }); continue; }
            }
            var lane = Math.min(pp.x, fl.x) - 8 - (laneSeq % 5) * 6; laneSeq++;
            try {
                await eda.sch_PrimitiveWire.create([pp.x, pp.y, lane, pp.y, lane, fl.y, fl.x, fl.y], '');
                L({ des: des2, pin: pk, pwr: short(t.p), to: K(fl.x, fl.y) });
            } catch (e) { L({ des: des2, pin: pk, routeErr: String(e) }); }
        }
    }
}

try { await eda.sch_Document.save(); L({ saved: true }); } catch (e) { L({ saveErr: String(e) }); }
out.wireTotal = ((await eda.sch_PrimitiveWire.getAll()) || []).length;
out.compTotal = ((await eda.sch_PrimitiveComponent.getAll()) || []).length;
return JSON.stringify(out, null, 1);
