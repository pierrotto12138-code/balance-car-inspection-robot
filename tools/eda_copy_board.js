// 复制"改"板做沙盒
var out = { before: [], after: [] };
try { await eda.dmt_EditorControl.openDocument('c405558037e65ebd'); } catch (e) { }
try { await eda.dmt_Project.openProject('d9cfd07ad0697eaf31cc900a3293f48c008aeacd38c0a14eae824bb8bdcdbf26'); } catch (e) { }
try {
    var b0 = await eda.dmt_Board.getAllBoardsInfo();
    out.before = (b0 || []).map(function (x) { return { name: x.name, uuid: x.uuid }; });
} catch (e) { out.beforeErr = String(e); }
try {
    var r = await eda.dmt_Board.copyBoard('改');
    out.copyResult = r;
} catch (e) { out.copyErr = String(e); }
try {
    var b1 = await eda.dmt_Board.getAllBoardsInfo();
    out.after = (b1 || []).map(function (x) { return { name: x.name, uuid: x.uuid }; });
} catch (e) { out.afterErr = String(e); }
return JSON.stringify(out, null, 1);
