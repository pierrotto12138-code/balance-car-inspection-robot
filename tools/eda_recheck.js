// 复检：Netlist Error / 走线统计 / 未连接网络 / 间距违规
var out = { connNets: [], top: [], widths: {}, layers: {} };
try { await eda.dmt_EditorControl.openDocument('07ed4147375824d3'); } catch (e) { out.openErr = String(e); }
var lines = (await eda.pcb_PrimitiveLine.getAll()) || [];
out.lineCount = lines.length;
var netsWithTraces = {};
for (var i = 0; i < lines.length; i++) {
    var w = String(lines[i].lineWidth), ly = String(lines[i].layer);
    out.widths[w] = (out.widths[w] || 0) + 1;
    out.layers[ly] = (out.layers[ly] || 0) + 1;
    netsWithTraces[String(lines[i].net)] = 1;
}
out.tracedNets = Object.keys(netsWithTraces);
out.tracedCount = out.tracedNets.length;
try { out.vias = ((await eda.pcb_PrimitiveVia.getAll()) || []).length; } catch (e) { }
try { out.pours = ((await eda.pcb_PrimitivePour.getAll()) || []).length; } catch (e) { }
// 各网络的走线宽度明细（看有没有该粗没粗的）
var netW = {};
for (var j = 0; j < lines.length; j++) {
    var nn = String(lines[j].net);
    (netW[nn] = netW[nn] || {});
    var ww = String(lines[j].lineWidth);
    netW[nn][ww] = (netW[nn][ww] || 0) + 1;
}
out.netWidths = netW;
// DRC
try {
    var r = await eda.pcb_Drc.check(true, false, true);
    (function walk(n, dep) {
        if (!n || dep > 5) return;
        if (Array.isArray(n)) { for (var i = 0; i < n.length; i++) walk(n[i], dep + 1); return; }
        var t = n.title !== undefined ? (typeof n.title === 'object' ? String(n.title[0]) : String(n.title)) : null;
        if (t && n.count !== undefined && dep <= 2) out.top.push(t + ' = ' + n.count);
        // 顶层分类下的子项
        if (t && n.list && Array.isArray(n.list) && dep <= 2) {
            for (var k = 0; k < n.list.length; k++) {
                var it = n.list[k];
                if (!it) continue;
                var itt = it.title !== undefined ? (typeof it.title === 'object' ? String(it.title[0]) : String(it.title)) : null;
                if (itt && it.count !== undefined) out.connNets.push(itt + ':' + it.count);
            }
        }
        if (n.list) walk(n.list, dep + 1);
    })(r, 0);
    var s = JSON.stringify(r);
    out.hasNetlistErr = /Netlist Error/i.test(s);
    out.hasClearance = /Clearance|Spacing/i.test(s);
} catch (e) { out.drcErr = String(e); }
var seen = {}, u = [];
for (var q = 0; q < out.connNets.length; q++) { if (!seen[out.connNets[q]]) { seen[out.connNets[q]] = 1; u.push(out.connNets[q]); } }
out.connNets = u;
return JSON.stringify(out, null, 1);
