// 统计：改板 40 个元件里，有多少带"供应商料号"（立创商城 C 编号）与封装
var out = {};
try { await eda.dmt_EditorControl.openDocument('c405558037e65ebd'); } catch (e) { out.openErr = String(e); }

function desOf(c) { try { return c.getState_Designator ? (c.getState_Designator() || '') : ''; } catch (e) { return ''; } }
function safe(fn) { try { return fn(); } catch (e) { return null; } }

var comps = (await eda.sch_PrimitiveComponent.getAll()) || [];
var withSupplier = [], withFootprint = [], withOriginFp = [], noValue = [];
var total = 0;

for (var i = 0; i < comps.length; i++) {
    var c = comps[i];
    var d = desOf(c);
    if (!d) continue;
    total++;
    var sup = safe(function () { return c.getState_SupplierId ? c.getState_SupplierId() : null; });
    var supName = safe(function () { return c.getState_Supplier ? c.getState_Supplier() : null; });
    var fp = safe(function () { return c.getState_Footprint ? c.getState_Footprint() : null; });
    var op = safe(function () { return c.getState_OtherProperty ? c.getState_OtherProperty() : null; });
    if (sup || supName) withSupplier.push({ des: d, supplier: supName, id: sup });
    if (fp && fp.uuid) withFootprint.push({ des: d, name: fp.name });
    if (op && op['Origin Footprint']) withOriginFp.push({ des: d, fp: op['Origin Footprint'] });
    if (op && !op['Value']) noValue.push(d);
}

out.totalDesignators = total;
out.withSupplierCount = withSupplier.length;
out.withSupplier = withSupplier;
out.withFootprintCount = withFootprint.length;
out.withOriginFootprintCount = withOriginFp.length;
out.sampleOriginFootprints = withOriginFp.slice(0, 30);
out.designatorsWithoutValue = noValue;

return JSON.stringify(out, null, 1);
