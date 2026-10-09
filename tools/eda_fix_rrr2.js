// 精修：R13 引线方向、R14 的 +3.3V 符号错位
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
function flagNet(c) { try { var n = c.net || ''; return n.indexOf('POWER_') === 0 ? n : null; } catch (e) { return null; } }

// --- 1) 删 R11/R13 那一带的信号引线（合并区域，确保整条都在区内） ---
var ws = (await eda.sch_PrimitiveWire.getAll()) || [];
var del = [];
for (var w = 0; w < ws.length; w++) {
    var vp = ptsOf(ws[w].line);
    if (!vp.length) continue;
    var ins = true;
    for (var v = 0; v < vp.length; v++) {
        if (!(vp[v][0] >= 870 && vp[v][0] <= 1000 && vp[v][1] >= 618 && vp[v][1] <= 642)) { ins = false; break; }
    }
    if (ins) del.push(ws[w].primitiveId);
}
if (del.length) { try { L({ delR11R13Stubs: del.length, ok: await eda.sch_PrimitiveWire.delete(del) }); } catch (e) { L({ e1: String(e) }); } }

// --- 2) 删 R14 那一带的导线 ---
var ws2 = (await eda.sch_PrimitiveWire.getAll()) || [];
var del2 = [];
for (var w2 = 0; w2 < ws2.length; w2++) {
    var v2 = ptsOf(ws2[w2].line);
    if (!v2.length) continue;
    var ins2 = true;
    for (var q = 0; q < v2.length; q++) {
        if (!(v2[q][0] >= 640 && v2[q][0] <= 800 && v2[q][1] >= 580 && v2[q][1] <= 650)) { ins2 = false; break; }
    }
    if (ins2) del2.push(ws2[w2].primitiveId);
}
if (del2.length) { try { L({ delR14Wires: del2.length, ok: await eda.sch_PrimitiveWire.delete(del2) }); } catch (e) { L({ e2: String(e) }); } }

// --- 3) 删掉压在 R14 脚上的那个 +3.3V 符号 ---
var comps = (await eda.sch_PrimitiveComponent.getAll()) || [];
var victim = null;
for (var i = 0; i < comps.length; i++) {
    if (flagNet(comps[i]) === 'POWER_+3.3V' && Math.abs(comps[i].x - 695) < 25 && Math.abs(comps[i].y - 600) < 25) {
        victim = comps[i]; break;
    }
}
if (victim) {
    try { L({ deletedStrayFlag: K(victim.x, victim.y), ok: await eda.sch_PrimitiveComponent.delete(victim.primitiveId) }); }
    catch (e) { L({ delFlagErr: String(e) }); }
} else L({ note: '未找到压在 R14 脚上的 +3.3V 符号' });

// --- 4) 重建 ---
async function pinsOf(des) {
    var cs = (await eda.sch_PrimitiveComponent.getAll()) || [];
    for (var i = 0; i < cs.length; i++) {
        if (desOf(cs[i]) === des) {
            var ps = (await cs[i].getAllPins()) || [];
            var m = {};
            for (var j = 0; j < ps.length; j++) m[String(ps[j].pinNumber)] = { x: ps[j].x, y: ps[j].y };
            return m;
        }
    }
    return null;
}

// R11 pin1：向右引（远离 R13）
var p11 = await pinsOf('R11');
if (p11) {
    try { await eda.sch_PrimitiveWire.create([p11['1'].x, p11['1'].y, p11['1'].x + 30, p11['1'].y], 'PA5'); L({ R11: 'pin1 -> 右 PA5' }); }
    catch (e) { L({ R11e: String(e) }); }
}
// R13 pin1：向左引
var p13 = await pinsOf('R13');
if (p13) {
    try { await eda.sch_PrimitiveWire.create([p13['1'].x, p13['1'].y, p13['1'].x - 30, p13['1'].y], 'PA6'); L({ R13: 'pin1 -> 左 PA6' }); }
    catch (e) { L({ R13e: String(e) }); }
}
// R14 pin1：向左引 PA7；pin2：向右引到新的 +3.3V 符号
var p14 = await pinsOf('R14');
if (p14) {
    try { await eda.sch_PrimitiveWire.create([p14['1'].x, p14['1'].y, p14['1'].x - 30, p14['1'].y], 'PA7'); L({ R14: 'pin1 -> 左 PA7' }); }
    catch (e) { L({ R14e1: String(e) }); }
    var fx = p14['2'].x + 35, fy = p14['2'].y;
    try {
        await eda.sch_PrimitiveWire.create([p14['2'].x, p14['2'].y, fx, fy], '');
        var f = await eda.sch_PrimitiveComponent.createNetFlag('Power', 'POWER_+3.3V', fx, fy);
        L({ R14: 'pin2 -> +3.3V @' + K(fx, fy), ok: !!f });
    } catch (e) { L({ R14e2: String(e) }); }
}
try { await eda.sch_Document.save(); L({ saved: true }); } catch (e) { L({ saveErr: String(e) }); }
return JSON.stringify(out, null, 1);
