// 用 uniqueId 把原理图与 PCB 的元件精确配对
var out = { sch: [], pcb: [] };
function desOf(c) { try { return c.getState_Designator ? (c.getState_Designator() || '') : ''; } catch (e) { return ''; } }
function uidOf(c) { try { return c.getState_UniqueId ? c.getState_UniqueId() : ''; } catch (e) { return ''; } }
function devOf(c) { try { var d = c.getState_Component ? c.getState_Component() : null; return (d && d.name) || (d && d.uuid && d.uuid.name) || ''; } catch (e) { return ''; } }

try {
    await eda.dmt_EditorControl.openDocument('c405558037e65ebd');
    var sc = (await eda.sch_PrimitiveComponent.getAll()) || [];
    for (var i = 0; i < sc.length; i++) { var d = desOf(sc[i]); if (d && d.indexOf('#PWR') !== 0) out.sch.push({ des: d, uid: uidOf(sc[i]), dev: devOf(sc[i]) }); }
} catch (e) { out.schErr = String(e); }
try {
    await eda.dmt_EditorControl.openDocument('07ed4147375824d3');
    var pc = (await eda.pcb_PrimitiveComponent.getAll()) || [];
    for (var m = 0; m < pc.length; m++) out.pcb.push({ des: desOf(pc[m]), uid: uidOf(pc[m]), dev: devOf(pc[m]), x: Math.round(pc[m].x), y: Math.round(pc[m].y) });
} catch (e) { out.pcbErr = String(e); }
return JSON.stringify(out, null, 1);
