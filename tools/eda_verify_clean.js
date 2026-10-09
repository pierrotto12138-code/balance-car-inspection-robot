// 复核：元件总数与位号清单是否正常（确认测试件已清干净）
var out = {};
try { await eda.dmt_EditorControl.openDocument('c405558037e65ebd'); } catch (e) { out.openErr = String(e); }

function desOf(c) { try { return c.getState_Designator ? (c.getState_Designator() || '') : ''; } catch (e) { return ''; } }
function devOf(c) { try { var d = c.getState_Component ? c.getState_Component() : null; return (d && d.name) || ''; } catch (e) { return ''; } }

var comps = (await eda.sch_PrimitiveComponent.getAll()) || [];
out.total = comps.length;

var named = [], pwr = [], nameless = [], faraway = [];
for (var i = 0; i < comps.length; i++) {
    var c = comps[i];
    var d = desOf(c);
    if (!d) { nameless.push({ dev: devOf(c), x: c.x, y: c.y, id: c.primitiveId }); continue; }
    if (d.indexOf('#PWR') === 0) pwr.push(d);
    else named.push(d);
    if (c.x > 1500 || c.y > 1500) faraway.push({ des: d, dev: devOf(c), x: c.x, y: c.y });
}
out.namedCount = named.length;
out.powerCount = pwr.length;
out.namelessCount = nameless.length;
out.nameless = nameless;
out.named = named.sort();
out.faraway = faraway;   // 应只剩测试残留（应为空）

return JSON.stringify(out, null, 1);
