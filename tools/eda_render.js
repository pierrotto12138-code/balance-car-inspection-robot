// 渲染当前原理图区为 PNG（返回 dataURL）
try { await eda.dmt_EditorControl.openDocument('c405558037e65ebd'); } catch (e) { }
var blob = await eda.dmt_EditorControl.getCurrentRenderedAreaImage();
if (!blob) return 'NO_BLOB';
var dataUrl = await new Promise(function (resolve, reject) {
    var fr = new FileReader();
    fr.onload = function () { resolve(String(fr.result)); };
    fr.onerror = function (e) { reject(e); };
    fr.readAsDataURL(blob);
});
return dataUrl;
