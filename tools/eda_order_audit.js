// 下单相关审计：封装（实例级/器件级）+ 立创料号 + 厂商
var out = { rows: [] };
try { await eda.dmt_EditorControl.openDocument('c405558037e65ebd'); } catch (e) { out.openErr = String(e); }
function desOf(c) { try { return c.getState_Designator ? (c.getState_Designator() || '') : ''; } catch (e) { return ''; } }
function safe(fn) { try { return fn(); } catch (e) { return null; } }

var comps = (await eda.sch_PrimitiveComponent.getAll()) || [];
for (var i = 0; i < comps.length; i++) {
    var c = comps[i];
    var d = desOf(c);
    if (!d || d.indexOf('#PWR') === 0) continue;
    var fp = safe(function () { return c.getState_Footprint(); });
    var op = safe(function () { return c.getState_OtherProperty(); }) || {};
    var sid = safe(function () { return c.getState_SupplierId ? c.getState_SupplierId() : null; });
    var sup = safe(function () { return c.getState_Supplier ? c.getState_Supplier() : null; });
    var dev = safe(function () { return c.getState_Component(); });
    out.rows.push({
        des: d,
        dev: (dev && dev.name) || '',
        fpName: (fp && fp.name) || '',
        fpLinked: !!(fp && fp.uuid),
        originFp: op['Origin Footprint'] || '',
        lcsc: op['Supplier Part'] || op['LCSC Part Name'] || sid || '',
        lcscName: op['LCSC Part Name'] || '',
        supplier: sup || op['Supplier'] || '',
        mfr: op['Manufacturer'] || ''
    });
}
out.total = out.rows.length;
out.fpMissing = out.rows.filter(function (r) { return !r.fpLinked; }).length;
out.lcscMissing = out.rows.filter(function (r) { return !r.lcsc; }).length;
return JSON.stringify(out, null, 1);
