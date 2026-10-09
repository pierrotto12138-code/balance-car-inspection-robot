// 统计"实例级封装为空"的元件，验证是否 = 18（对应 18 个 fatalError）
var out = { noFp: [], noFpCount: 0, withFp: 0 };
try { await eda.dmt_EditorControl.openDocument('c405558037e65ebd'); } catch (e) { }
function desOf(c) { try { return c.getState_Designator ? (c.getState_Designator() || '') : ''; } catch (e) { return ''; } }
var comps = (await eda.sch_PrimitiveComponent.getAll()) || [];
for (var i = 0; i < comps.length; i++) {
    var c = comps[i], d = desOf(c);
    if (!d || d.indexOf('#PWR') === 0) continue;
    var fp = null;
    try { fp = c.getState_Footprint(); } catch (e) { }
    var linked = !!(fp && fp.uuid);
    if (linked) out.withFp++;
    else {
        var op = null, dev = null;
        try { op = c.getState_OtherProperty(); } catch (e) { }
        try { var dv = c.getState_Component(); dev = dv && dv.name; } catch (e) { }
        out.noFp.push({ des: d, dev: dev, originFp: (op && op['Origin Footprint']) || '', x: c.x, y: c.y });
    }
}
out.noFpCount = out.noFp.length;
out.noFpDes = out.noFp.map(function (z) { return z.des; }).sort();
return JSON.stringify(out, null, 1);
