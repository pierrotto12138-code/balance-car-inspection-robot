// 跑原理图 DRC 并返回明细；同时看 PCB2 是否已经有元件
var out = {};
try { await eda.dmt_EditorControl.openDocument('c405558037e65ebd'); } catch (e) { }
try {
    var r = await eda.sch_Drc.check(true, false, true);
    out.drc = r;
    out.drcType = Object.prototype.toString.call(r);
} catch (e) { out.drcErr = String(e); }
// 再试一次不返回明细的，看是否通过
try { out.pass = await eda.sch_Drc.check(true, false, false); } catch (e) { out.passErr = String(e); }

// PCB2 现状
try {
    var pcbs = await eda.dmt_Pcb.getAllPcbs();
    out.pcbs = (pcbs || []).map(function (p) { return { name: p.name, uuid: p.uuid }; });
} catch (e) { out.pcbErr = String(e); }
try {
    var info = await eda.dmt_Pcb.getCurrentPcbInfo();
    out.currentPcb = info && { name: info.name, uuid: info.uuid };
} catch (e) { }
try {
    var comps = await eda.pcb_PrimitiveComponent.getAll();
    out.pcbCompCount = (comps || []).length;
} catch (e) { out.pcbCompErr = String(e); }
return JSON.stringify(out, null, 1);
