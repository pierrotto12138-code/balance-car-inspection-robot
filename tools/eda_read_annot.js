// 读"原"板(Schematic1)上的模块标注文字，并给出每条文字最近的座
var out = {};
try { await eda.dmt_EditorControl.openDocument('b3b927a3baeee945'); } catch (e) { out.openErr = String(e); }

function K(x, y) { return Math.round(x) + ',' + Math.round(y); }
function desOf(c) { try { return c.getState_Designator ? (c.getState_Designator() || '') : ''; } catch (e) { return ''; } }
function devOf(c) { try { var d = c.getState_Component ? c.getState_Component() : null; return (d && d.name) || ''; } catch (e) { return ''; } }

var texts = [];
try {
    var ts = (await eda.sch_PrimitiveText.getAll()) || [];
    texts = ts.map(function (t) {
        return { text: t.text || t.value || t.content || '', x: t.x, y: t.y };
    }).filter(function (t) { return t.text; });
} catch (e) { out.textErr = String(e); }
out.textCount = texts.length;

// 元件（用于找"最近的座"）
var comps = (await eda.sch_PrimitiveComponent.getAll()) || [];
var socks = [];
for (var i = 0; i < comps.length; i++) {
    var d = desOf(comps[i]);
    if (d && d.charAt(0) === 'J') socks.push({ des: d, x: comps[i].x, y: comps[i].y, dev: devOf(comps[i]) });
}

// 每条文字找最近的座
out.annotated = texts.map(function (t) {
    var best = null, bd = 1e9;
    for (var j = 0; j < socks.length; j++) {
        var dd = Math.abs(socks[j].x - t.x) + Math.abs(socks[j].y - t.y);
        if (dd < bd) { bd = dd; best = socks[j]; }
    }
    return { text: t.text, at: K(t.x, t.y), nearest: best ? best.des : null, dist: bd, dev: best ? best.dev : null };
});
out.rawTexts = texts;
out.sockets = socks;

return JSON.stringify(out, null, 1);
