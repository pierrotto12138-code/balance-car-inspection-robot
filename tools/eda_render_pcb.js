var out = "NO_BLOB";
try { await eda.dmt_EditorControl.openDocument("07ed4147375824d3"); } catch (e) { }
var blob = await eda.dmt_EditorControl.getCurrentRenderedAreaImage();
if (blob) {
  out = await new Promise(function (res, rej) {
    var fr = new FileReader();
    fr.onload = function () { res(String(fr.result)); };
    fr.onerror = function (e) { rej(e); };
    fr.readAsDataURL(blob);
  });
}
return out;
