// 搜索各所需阻值/容值的 0805 候选器件（只读），供挑选
var out = { groups: [] };
try { await eda.dmt_EditorControl.openDocument('c405558037e65ebd'); } catch (e) { out.openErr = String(e); }

var SYS = '0819f05c4eef4c71ace90d822a990e87';

function brief(it) {
    return {
        name: it.name,
        fp: it.footprintName,
        desc: (it.description || '').slice(0, 40),
        mfr: it.manufacturer,
        sup: it.supplierId || it.supplier,
        props: it.otherProperty || null
    };
}

var queries = [
    { tag: 'R 10k 0805', q: '10K 0805 电阻', fp: 'R0805' },
    { tag: 'R 15k 0805', q: '15K 0805 电阻', fp: 'R0805' },
    { tag: 'R 4.7k 0805', q: '4.7K 0805 电阻', fp: 'R0805' },
    { tag: 'C 0.1uF 0805', q: '100nF 0805 电容', fp: 'C0805' },
    { tag: 'C 100uF', q: '100uF 25V 电解电容', fp: null }
];

for (var i = 0; i < queries.length; i++) {
    var rec = { tag: queries[i].tag, q: queries[i].q, hits: [] };
    try {
        var r = await eda.lib_Device.search(queries[i].q, SYS);
        r = r || [];
        for (var j = 0; j < r.length; j++) {
            var it = r[j];
            if (queries[i].fp && it.footprintName !== queries[i].fp) continue;
            rec.hits.push(brief(it));
        }
        rec.rawCount = r.length;
    } catch (e) { rec.err = String(e); }
    out.groups.push(rec);
}
return JSON.stringify(out, null, 1);
