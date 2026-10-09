// 按 uniqueId 把 PCB 位号改回与原理图一致
var out = { log: [], changed: [] };
function desOf(c) { try { return c.getState_Designator ? (c.getState_Designator() || '') : ''; } catch (e) { return ''; } }
function uidOf(c) { try { return c.getState_UniqueId ? c.getState_UniqueId() : ''; } catch (e) { return ''; } }

// 1) 原理图：uid -> 位号
var want = {};
try {
    await eda.dmt_EditorControl.openDocument('c405558037e65ebd');
    var sc = (await eda.sch_PrimitiveComponent.getAll()) || [];
    for (var i = 0; i < sc.length; i++) {
        var d = desOf(sc[i]), u = uidOf(sc[i]);
        if (d && u && d.indexOf('#PWR') !== 0) want[u] = d;
    }
} catch (e) { out.log.push({ schErr: String(e) }); }
out.log.push({ schMap: Object.keys(want).length });

// 2) PCB：按 uid 找到需要改的
try {
    await eda.dmt_EditorControl.openDocument('07ed4147375824d3');
    var pc = (await eda.pcb_PrimitiveComponent.getAll()) || [];
    out.log.push({ pcbCount: (pc || []).length });
    for (var m = 0; m < pc.length; m++) {
        var c = pc[m], u2 = uidOf(c), cur = desOf(c), target = want[u2];
        if (!target || target === cur) continue;
        try {
            var r = await eda.pcb_PrimitiveComponent.modify(c.primitiveId, { designator: target });
            var back = (r && r.getState_Designator) ? r.getState_Designator() : null;
            out.changed.push({ from: cur, to: target, readBack: back, uid: u2 });
        } catch (e) {
            out.changed.push({ from: cur, to: target, err: String(e) });
        }
    }
} catch (e) { out.log.push({ pcbErr: String(e) }); }

try { await eda.pcb_Document.save(); out.log.push({ saved: true }); } catch (e) { out.log.push({ saveErr: String(e) }); }
return JSON.stringify(out, null, 1);
