// 给 U1/U2/U3 写入立创料号（用 modify 的 supplier/supplierId，不替换器件 → 不会弄断导线）
var out = { log: [] };
try { await eda.dmt_EditorControl.openDocument('c405558037e65ebd'); } catch (e) { out.openErr = String(e); }
function desOf(c) { try { return c.getState_Designator ? (c.getState_Designator() || '') : ''; } catch (e) { return ''; } }

var PLAN = {
    U1: { supplier: 'LCSC', supplierId: 'C88224' },      // TB6612FNG(O,C,8,EL) SSOP-24 东芝
    U2: { supplier: 'LCSC', supplierId: 'C2980820' },    // LM2596S-5.0 TO-263-5 萨科微
    U3: { supplier: 'LCSC', supplierId: 'C2891839' }     // AMS1117-3.3 SOT-223 永裕泰
};

var comps = (await eda.sch_PrimitiveComponent.getAll()) || [];
for (var des in PLAN) {
    var me = null;
    for (var i = 0; i < comps.length; i++) if (desOf(comps[i]) === des) { me = comps[i]; break; }
    if (!me) { out.log.push({ des: des, err: 'not found' }); continue; }
    var before = { fp: null };
    try { var f0 = me.getState_Footprint(); before.fp = f0 && f0.name; } catch (e) { }
    try {
        var r = await eda.sch_PrimitiveComponent.modify(me.primitiveId, { supplier: PLAN[des].supplier, supplierId: PLAN[des].supplierId });
        var o = { des: des, ok: !!r, set: PLAN[des].supplierId };
        if (r) {
            try { o.supplierId = r.getState_SupplierId(); } catch (e) { }
            try { o.supplier = r.getState_Supplier(); } catch (e) { }
            try { var f1 = r.getState_Footprint(); o.fpAfter = f1 && f1.name; } catch (e) { }
            try { var d1 = r.getState_Component(); o.dev = d1 && d1.name; } catch (e) { }
        }
        o.fpBefore = before.fp;
        out.log.push(o);
    } catch (e) { out.log.push({ des: des, err: String(e) }); }
}
try { await eda.sch_Document.save(); out.log.push({ saved: true }); } catch (e) { }
return JSON.stringify(out, null, 1);
