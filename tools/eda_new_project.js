// 新建工程 "26测控系统设计"
var out = {};
try { var p0 = await eda.dmt_Project.getCurrentProjectInfo(); out.before = p0 ? { name: p0.name, uuid: p0.uuid } : null; } catch (e) { out.bErr = String(e); }
try {
    var u = await eda.dmt_Project.createProject('26测控系统设计', '26测控系统设计');
    out.created = u;
} catch (e) { out.createErr = String(e); }
try { var p1 = await eda.dmt_Project.getCurrentProjectInfo(); out.after = p1 ? { name: p1.name, uuid: p1.uuid } : null; } catch (e) { out.aErr = String(e); }
try {
    var ids = await eda.dmt_Project.getAllProjectsUuid();
    out.projectCount = (ids || []).length;
} catch (e) { }
try { var b = await eda.dmt_Board.getAllBoardsInfo(); out.boards = (b || []).map(function (x) { return x.name; }); } catch (e) { out.boardErr = String(e); }
return JSON.stringify(out, null, 1);
