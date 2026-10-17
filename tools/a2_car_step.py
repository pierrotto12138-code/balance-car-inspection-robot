# -*- coding: utf-8 -*-
r"""把整车导出为一个 STEP（真 B-rep 实体，供云 CAD / SolidWorks 打开）。

组成（全部以实体形式进同一个 STEP）：
  · 本组外壳：下壳 / 上壳（读已有的 STEP）
  · 本组主控板：3D_PCB2 STEP（53 个实体，按已知平移放到壳坐标）
  · 参考件：电机 ×2（旋转/平移就位）、电池 ×1（绕 Z 转 90°）
  · 参数化件：底盘板、车轮 ×2、M3 铜柱 ×4（现场生成实体）

坐标：原点 = 板左下角，板面 z = 0，元件面 +z；单位 mm。
若 OpenCASCADE 的 XCAF 可用，则写成**带零件名与装配结构**的 STEP（AP242）。
"""
import math, os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from OCP.BRepPrimAPI import BRepPrimAPI_MakeBox, BRepPrimAPI_MakeCylinder
from OCP.BRepAlgoAPI import BRepAlgoAPI_Cut, BRepAlgoAPI_Fuse
from OCP.BRepBuilderAPI import BRepBuilderAPI_Transform, BRepBuilderAPI_Copy
from OCP.gp import gp_Pnt, gp_Vec, gp_Ax2, gp_Ax1, gp_Dir, gp_Trsf
from OCP.STEPControl import STEPControl_Writer, STEPControl_AsIs
from OCP.STEPControl import STEPControl_Reader
from OCP.IFSelect import IFSelect_RetDone
from OCP.TopoDS import TopoDS, TopoDS_Shape
from OCP.TopExp import TopExp_Explorer
from OCP.TopAbs import TopAbs_SOLID
from OCP.Bnd import Bnd_Box
from OCP.BRepBndLib import BRepBndLib

OUT = r"D:\260921Task_私有资料\正式作业提交\_A2审阅\外壳起点件"
REF = r"D:\260921Task_私有资料\作业补充\A2\小车_STEP"
STEP_PCB = r"D:\260921Task\hardware\eda_export_20261016\3D_PCB2_2026-10-16.step"

# ---- 与 a2_shell.py / a2_car_assembly.py 一致的参数
MARGIN, WALL = 3.0, 2.4
W, H = 100 + 2 * (MARGIN + WALL), 65 + 2 * (MARGIN + WALL)
OX = OY = MARGIN + WALL
STAND_H, FLOOR = 6.6, 2.0
Z_BOTTOM = -(STAND_H + FLOOR)
STANDOFF_H = 25.0
Z_CHASSIS_TOP = Z_BOTTOM - STANDOFF_H
CHASSIS_T = 4.0
WHEEL_D, WHEEL_W = 67.0, 26.0
Z_WHEEL = Z_CHASSIS_TOP - CHASSIS_T / 2
CHASSIS_L, CHASSIS_Wd = 110.0, 116.0
CX, CY = OX + 50, OY + 32.5


def trsf_t(dx, dy, dz):
    t = gp_Trsf(); t.SetTranslation(gp_Vec(dx, dy, dz)); return t


def trsf_rz(deg):
    t = gp_Trsf(); t.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), math.radians(deg)); return t


def xform(shape, t):
    return BRepBuilderAPI_Transform(shape, t, True).Shape()


def read_step(path):
    r = STEPControl_Reader()
    if r.ReadFile(path) != IFSelect_RetDone:
        raise RuntimeError(f"读不了 {path}")
    r.TransferRoots()
    return r.OneShape()


def count_solids(shape):
    n = 0; ex = TopExp_Explorer(shape, TopAbs_SOLID)
    while ex.More():
        n += 1; ex.Next()
    return n


def bbox(shape):
    b = Bnd_Box(); BRepBndLib.Add_s(shape, b)
    mn, mx = b.CornerMin(), b.CornerMax()
    return (mn.X(), mn.Y(), mn.Z(), mx.X(), mx.Y(), mx.Z())


parts = []      # (name, shape, (r,g,b))
PALETTE = {"下壳": (0.55, 0.72, 0.87), "上壳": (0.80, 0.86, 0.65), "主控板": (0.25, 0.55, 0.35),
           "电机": (0.35, 0.35, 0.38), "电池": (0.85, 0.55, 0.30), "底盘": (0.60, 0.60, 0.62),
           "车轮": (0.22, 0.22, 0.24), "铜柱": (0.85, 0.68, 0.30), "控制板": (0.30, 0.45, 0.70)}


NAME_MAP = {"下壳": "lower_shell", "上壳": "upper_shell",
            "主控板": "pcb_board", "主控板(板框)": "pcb_board_outline",
            "控制板叠层(包络)": "ctrl_board_env", "电池": "battery_pack",
            "底盘板": "chassis_plate", "电机": "motor", "车轮": "wheel",
            "铜柱": "standoff", "控制板": "ctrl_board"}


def ascii_name(name):
    if name in NAME_MAP:
        return NAME_MAP[name]
    for k, v in NAME_MAP.items():
        if name.startswith(k):
            return v
    return "".join(ch if (ch.isascii() and ch.isalnum()) else "_" for ch in name).strip("_") or "part"


def color_of(name):
    for k, v in PALETTE.items():
        if name.startswith(k):
            return v
    return (0.7, 0.7, 0.7)

# ① 本组外壳
for nm, fn in (("下壳", "A2-01_下壳.step"), ("上壳", "A2-02_上壳.step")):
    p = os.path.join(OUT, fn)
    if os.path.exists(p):
        s = read_step(p)
        parts.append((nm, s, color_of(nm)))
        print(f"  + {nm:<8} 实体 {count_solids(s):2d}  {np.round(np.array(bbox(s))[3:] - np.array(bbox(s))[:3], 1)}")

# ② 本组主控板（STEP 坐标 → 壳坐标：平移 (5.4+6.0, 5.4+58.91)）
LIGHT = "--light" in sys.argv
if LIGHT:
    # 轻量：板框实体（100×65×1.6）+ 控制板叠层包络盒
    b1 = BRepPrimAPI_MakeBox(gp_Pnt(OX, OY, 0), gp_Pnt(OX + 100, OY + 65, 1.6)).Shape()
    parts.append(("主控板(板框)", b1, color_of("主控板")))
    b2 = BRepPrimAPI_MakeBox(gp_Pnt(OX, OY, 13.3), gp_Pnt(OX + 100, OY + 65, 24.9)).Shape()
    parts.append(("控制板叠层(包络)", b2, color_of("控制板")))
    print(f"  + 主控板   轻量模式：板框实体 + 叠层包络盒（不含 149 个器件）")
elif os.path.exists(STEP_PCB):
    s = read_step(STEP_PCB)
    s = xform(s, trsf_t(OX + 6.00, OY + 58.91, 0))
    parts.append(("主控板", s, color_of("主控板")))
    print(f"  + 主控板   实体 {count_solids(s):2d}  {np.round(np.array(bbox(s))[3:] - np.array(bbox(s))[:3], 1)}")

# ③ 参考件：电机 ×2（轴沿 X）、电池 ×1（绕 Z 90°，沿 Y 摆）
def ref_shape(fn):
    p = os.path.join(REF, fn)
    return read_step(p) if os.path.exists(p) else None


mot = ref_shape("____________.stp.step")
if mot is not None:
    _b = bbox(mot); mn, mx = np.array(_b[:3]), np.array(_b[3:])
    c = ((mn + mx) / 2).tolist()
    base = xform(mot, trsf_t(-c[0], -c[1], -c[2]))          # 居中到原点
    for k, sx in enumerate((-1, 1)):
        xc = CX + sx * (CHASSIS_L / 2 - 16)
        s = xform(base, trsf_t(xc, CY, Z_CHASSIS_TOP + 6 + (mx[2] - mn[2]) / 2))
        parts.append((f"电机{k+1}", s, color_of("电机")))
    print(f"  + 电机 ×2  实体 {count_solids(parts[-1][1]):2d}")

# 电池：参考件是曲面（0 实体），按包络 94.5×28.5×13.2 建实体盒（长边沿 Y = 前进方向）
BAT = (28.5, 94.5, 13.2)          # X, Y, Z
s = BRepPrimAPI_MakeBox(gp_Pnt(CX - BAT[0] / 2, CY - BAT[1] / 2, Z_CHASSIS_TOP + 3),
                        gp_Pnt(CX + BAT[0] / 2, CY + BAT[1] / 2, Z_CHASSIS_TOP + 3 + BAT[2])).Shape()
parts.append(("电池", s, color_of("电池")))
print(f"  + 电池     实体 {count_solids(s):2d}（按包络 94.5×28.5×13.2 建实体盒）")

# ④ 参数化件：底盘板 / 车轮 / 铜柱
chassis = BRepPrimAPI_MakeBox(gp_Pnt(CX - CHASSIS_L / 2, CY - CHASSIS_Wd / 2, Z_CHASSIS_TOP - CHASSIS_T),
                              gp_Pnt(CX + CHASSIS_L / 2, CY + CHASSIS_Wd / 2, Z_CHASSIS_TOP)).Shape()
for i in range(4):
    x = CX - CHASSIS_L / 2 + 22 + i * 22
    for y in (CY - 40, CY + 40):
        chassis = BRepAlgoAPI_Cut(chassis, BRepPrimAPI_MakeCylinder(
            gp_Ax2(gp_Pnt(x, y, Z_CHASSIS_TOP - CHASSIS_T - 1), gp_Dir(0, 0, 1)), 6.0, CHASSIS_T + 2).Shape()).Shape()
for sx in (-1, 1):
    xc = CX + sx * (CHASSIS_L / 2 - 14)
    chassis = BRepAlgoAPI_Fuse(chassis, BRepPrimAPI_MakeBox(
        gp_Pnt(xc - 14, CY - 26, Z_CHASSIS_TOP), gp_Pnt(xc + 14, CY + 26, Z_CHASSIS_TOP + 6)).Shape()).Shape()
parts.append(("底盘板", chassis, color_of("底盘")))
print(f"  + 底盘板   实体 {count_solids(chassis):2d}")

for k, xc in enumerate((-(WHEEL_W / 2) - 0.5, W + WHEEL_W / 2 + 0.5)):
    w = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(xc - WHEEL_W / 2, CY, Z_WHEEL), gp_Dir(1, 0, 0)),
                                 WHEEL_D / 2, WHEEL_W).Shape()
    hub = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(xc - (WHEEL_W + 8) / 2, CY, Z_WHEEL), gp_Dir(1, 0, 0)),
                                   9.0, WHEEL_W + 8).Shape()
    w = BRepAlgoAPI_Fuse(w, hub).Shape()
    for j in range(6):
        a = j * math.pi / 3
        hy, hz = CY + 19 * math.cos(a), Z_WHEEL + 19 * math.sin(a)
        w = BRepAlgoAPI_Cut(w, BRepPrimAPI_MakeCylinder(
            gp_Ax2(gp_Pnt(xc - 80, hy, hz), gp_Dir(1, 0, 0)), 5.0, 200).Shape()).Shape()
    w = BRepAlgoAPI_Cut(w, BRepPrimAPI_MakeCylinder(
        gp_Ax2(gp_Pnt(xc - 80, CY, Z_WHEEL), gp_Dir(1, 0, 0)), 4.0, 200).Shape()).Shape()
    parts.append((f"车轮{k+1}", w, color_of("车轮")))
print(f"  + 车轮 ×2  实体 {count_solids(parts[-1][1]):2d}")

for k, (x, y) in enumerate([(1.7, 1.7), (W - 1.7, 1.7), (1.7, H - 1.7), (W - 1.7, H - 1.7)]):
    s = BRepAlgoAPI_Fuse(
        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(x, y, Z_CHASSIS_TOP), gp_Dir(0, 0, 1)), 3.0, STANDOFF_H).Shape(),
        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(x, y, Z_CHASSIS_TOP), gp_Dir(0, 0, 1)), 5.5, 3.0).Shape()).Shape()
    parts.append((f"铜柱{k+1}", s, color_of("铜柱")))
print(f"  + 铜柱 ×4")

# ---- 合成一个 shape 并写 STEP（尽量用 XCAF 带名字）
XCAF = "--xcaf" in sys.argv
base = "轻量" if LIGHT else "完整"
out = os.path.join(OUT, f"A2-10_整车_装配_{base}.step")

if XCAF:
    from OCP.TDocStd import TDocStd_Document
    from OCP.TCollection import TCollection_ExtendedString
    from OCP.XCAFDoc import XCAFDoc_DocumentTool, XCAFDoc_ColorGen
    from OCP.TDataStd import TDataStd_Name
    from OCP.Quantity import Quantity_Color, Quantity_TOC_RGB
    from OCP.STEPCAFControl import STEPCAFControl_Writer
    from OCP.STEPControl import STEPControl_AsIs as _AsIs
    doc = TDocStd_Document(TCollection_ExtendedString("MDTV-XCAF"))
    _main = doc.Main()                        # ★ OCP 里 ShapeTool_s 要传 doc.Main()（TDF_Label）
    st = XCAFDoc_DocumentTool.ShapeTool_s(_main)
    ct = XCAFDoc_DocumentTool.ColorTool_s(_main)
    _cnt = {}
    for name, shape, rgb in parts:
        base = ascii_name(name)
        _cnt[base] = _cnt.get(base, 0) + 1
        idx = _cnt[base]
        nm = f"{base}_{idx}" if idx > 1 or base in ("motor", "wheel", "standoff") else base
        lab = st.AddShape(shape, False)
        TDataStd_Name.Set_s(lab, TCollection_ExtendedString(nm))
        ct.SetColor(lab, Quantity_Color(rgb[0], rgb[1], rgb[2], Quantity_TOC_RGB), XCAFDoc_ColorGen)
    w = STEPCAFControl_Writer()
    w.SetNameMode(True); w.SetColorMode(True); w.SetLayerMode(True)
    w.Transfer(doc, _AsIs)
    ok = w.Write(out)
    print(f"\n装配 STEP（XCAF 带名字/颜色）：{os.path.basename(out)}  写出={'OK' if ok else ok}  件数 {len(parts)}")
else:
    from OCP.BRep import BRep_Builder
    from OCP.TopoDS import TopoDS_Compound
    builder = BRep_Builder()
    comp = TopoDS_Compound()
    builder.MakeCompound(comp)
    for _, s, *rest in parts:
        builder.Add(comp, s)
    w = STEPControl_Writer()
    w.Transfer(comp, STEPControl_AsIs)
    ok = w.Write(out)
    b = bbox(comp)
    print(f"\n整车 STEP（复合体，无名字）：{os.path.basename(out)}  实体 {count_solids(comp)} 个  "
          f"包络 {b[3]-b[0]:.2f} × {b[4]-b[1]:.2f} × {b[5]-b[2]:.2f} mm")

print(f"  文件 {os.path.getsize(out)/1024/1024:.2f} MB")
if XCAF:
    print("  零件名（STEP 内为 ASCII，中文对照）：")
    _c2 = {}
    for nm, _, _ in parts:
        base = ascii_name(nm); _c2[base] = _c2.get(base, 0) + 1; idx = _c2[base]
        shown = f"{base}_{idx}" if idx > 1 or base in ("motor", "wheel", "standoff") else base
        print(f"    {shown:<22} ← {nm}")
