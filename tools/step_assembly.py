# -*- coding: utf-8 -*-
r"""把若干零件写成**真正的 STEP 装配**（有装配树，不是一堆并列实体）。

为什么单独做这个模块（2026-10-17 实测的坑）：
    旧写法 `ShapeTool.AddShape(shape, False)` 只是把每个件加成一个**自由件**，
    文件里 14 个 PRODUCT 各自独立、`NEXT_ASSEMBLY_USAGE_OCCURRENCE`(NAUO) = 0
    ⇒ SolidWorks / 云 CAD 打开看不到装配树（"装配"两字名不副实）。
    另一种写法（NewShape 空装配 + AddComponent）在这版 OCC 上**连几何都没写出去**
    （实测 PRODUCT=1 / NAUO=0 / MANIFOLD_SOLID_BREP=0）。

可用的写法（本模块，已验证 PRODUCT=n+1 / NAUO=n / BREP=n）：
    ① 把零件装进一个 `TopoDS_Compound`；
    ② `AddShape(compound, makeAssembly=True)` —— 它按 compound 的子件自动建装配结构；
    ③ 逐个组件 `GetReferredShape` 拿到**被引用件**的标签，在那上面设名字与颜色；
    ④ `STEPCAFControl_Writer`（NameMode/ColorMode 打开）落盘。

自检：`python tools\step_assembly.py` —— 造两个盒子写盘，再**按 STEP 文本**数
PRODUCT / NAUO / BREP 并读回名字，断言装配树真的存在且名字对得上。
"""
import os


def write_assembly(parts, path, doc_name="assembly"):
    """parts: [(name, TopoDS_Shape, (r,g,b) 或 None), ...] → 写带装配树的 STEP。

    返回 STEP 写入状态（期望 `IFSelect_ReturnStatus.IFSelect_RetDone`）。
    """
    from OCP.BRep import BRep_Builder
    from OCP.STEPCAFControl import STEPCAFControl_Writer
    from OCP.STEPControl import STEPControl_AsIs
    from OCP.TCollection import TCollection_ExtendedString
    from OCP.TDF import TDF_Label
    from OCP.TDataStd import TDataStd_Name
    from OCP.TDocStd import TDocStd_Document
    from OCP.TopoDS import TopoDS_Compound
    from OCP.XCAFDoc import XCAFDoc_ColorGen, XCAFDoc_DocumentTool

    if not parts:
        raise ValueError("parts 为空")

    comp = TopoDS_Compound()
    BRep_Builder().MakeCompound(comp)
    for _, shape, _rgb in parts:
        BRep_Builder().Add(comp, _single_solid(shape))

    doc = TDocStd_Document(TCollection_ExtendedString("MDTV-XCAF"))
    st = XCAFDoc_DocumentTool.ShapeTool_s(doc.Main())
    ct = XCAFDoc_DocumentTool.ColorTool_s(doc.Main())

    asm = st.AddShape(comp, True)                       # ② 自动建装配结构
    TDataStd_Name.Set_s(asm, TCollection_ExtendedString(doc_name))

    seq = _components(st, asm)
    if seq.Length() != len(parts):
        print(f"  ⚠ 装配组件数 {seq.Length()} ≠ 输入件数 {len(parts)}（可能有重复几何被合并）")

    for i in range(1, seq.Length() + 1):
        ref = TDF_Label()
        if not st.GetReferredShape_s(seq.Value(i), ref):
            continue
        name, _shape, rgb = parts[i - 1]
        TDataStd_Name.Set_s(ref, TCollection_ExtendedString(name))
        if rgb is not None:
            from OCP.Quantity import Quantity_Color, Quantity_TOC_RGB
            ct.SetColor(ref, Quantity_Color(rgb[0], rgb[1], rgb[2], Quantity_TOC_RGB), XCAFDoc_ColorGen)

    w = STEPCAFControl_Writer()
    w.SetNameMode(True)
    w.SetColorMode(True)
    w.SetLayerMode(True)
    w.Transfer(doc, STEPControl_AsIs)
    return w.Write(path)


def _single_solid(shape):
    """复合体里只有 1 个实体时返回那个实体 —— 否则装配树会多出一层 `SOLID` 子件。

    （2026-10-17 实测：直接塞复合体会写成 `lower_shell → SOLID` 两级组件，
     NAUO 变成件数的 2 倍。）
    """
    from OCP.TopAbs import TopAbs_SOLID
    from OCP.TopExp import TopExp_Explorer
    from OCP.TopoDS import TopoDS

    solids = []
    ex = TopExp_Explorer(shape, TopAbs_SOLID)
    while ex.More():
        solids.append(TopoDS.Solid(ex.Current()))
        ex.Next()
    return solids[0] if len(solids) == 1 else shape


def _components(st, asm):
    from OCP.collections import Sequence_TDF_Label

    seq = Sequence_TDF_Label()
    st.GetComponents_s(asm, seq)
    return seq


def step_counts(path):
    """按 STEP 文本数结构实体（独立于 OCC 的读取器）。"""
    t = open(path, encoding="utf-8", errors="replace").read()
    import re

    names = [b or a for a, b in re.findall(r"PRODUCT\s*\(\s*'([^']*)'\s*,\s*'([^']*)'", t)]
    return {
        "PRODUCT": t.count("PRODUCT("),
        "NAUO": t.count("NEXT_ASSEMBLY_USAGE_OCCURRENCE"),
        "BREP": t.count("MANIFOLD_SOLID_BREP"),
        "names": names,
    }


def describe(path):
    """打印/返回一份装配结构摘要，供脚本末尾统一报数。"""
    c = step_counts(path)
    ok = c["NAUO"] > 0
    print(f"    结构：PRODUCT {c['PRODUCT']} / NAUO {c['NAUO']} / 实体 {c['BREP']} "
          f"⇒ {'装配树存在 ✅' if ok else '无装配树 ❌'}")
    print(f"    件名：{', '.join(c['names'])}")
    return c


def selftest():
    from OCP.BRepPrimAPI import BRepPrimAPI_MakeBox
    from OCP.gp import gp_Pnt

    out = os.path.join(os.environ.get("TEMP", "."), "asm_selftest.step")
    parts = [
        ("box_a", BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), gp_Pnt(20, 10, 5)).Shape(), (0.8, 0.2, 0.2)),
        ("box_b", BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 5), gp_Pnt(20, 10, 9)).Shape(), (0.2, 0.2, 0.8)),
    ]
    print("写出：", write_assembly(parts, out, doc_name="selftest_asm"))
    c = describe(out)
    ok = c["NAUO"] == 2 and c["BREP"] == 2 and c["PRODUCT"] == 3 and c["names"][1:] == ["box_a", "box_b"]
    print("自检：", "✅ 通过" if ok else f"❌ 失败 {c}")
    if not ok:
        raise SystemExit(1)
    return True


if __name__ == "__main__":
    selftest()
