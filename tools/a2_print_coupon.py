# -*- coding: utf-8 -*-
r"""A2 孔位试印件（print coupon）—— 打整壳之前，先打这一块验孔位。

为什么单独做一块：
    整壳打印 8~12 h，孔位错 0.5 mm 就得重打。本件只复现下壳**最容易出错的接口**，
    30 min 打完、拿铜柱与 M3 自攻一试就知道对不对：
      ① 3 个主板安装柱 —— Φ7 外径 / Φ3.2 通孔，孔心跨度 **88.00 × 52.81**（对角 102.63）
      ② 4 个 M3 外凸耳 —— Φ7 外径 / Φ2.7 自攻预孔，中心自角点内移 **1.7**（跨距 107.40 × 72.40）
      ③ 内腔轮廓 —— 105.9 × 70.9 / R8 ⇒ 板（100 × 65 / R5）落进去四周留 **3.0** 间隙
      ④ 柱高 6.60 ⇒ 板坐上去以后，板底最低器件（SW2 5.60）还剩 1.0 余量

坐标：与外壳一致 —— 原点 = 板左下角，板面 z = 0，元件面 +z。本件只做 z ∈ [-8.6, +1.4] 的底板段。

用法：
    python tools\a2_print_coupon.py                     # 默认输出到 A2_SHELL_COUPON_OUT 或外壳目录
    python tools\a2_print_coupon.py -o <目录>
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from OCP.BRepAlgoAPI import BRepAlgoAPI_Cut, BRepAlgoAPI_Fuse
from OCP.BRepBuilderAPI import (BRepBuilderAPI_MakeEdge, BRepBuilderAPI_MakeFace,
                                BRepBuilderAPI_MakeWire, BRepBuilderAPI_Transform)
from OCP.BRepGProp import BRepGProp
from OCP.BRepPrimAPI import BRepPrimAPI_MakeBox, BRepPrimAPI_MakeCylinder, BRepPrimAPI_MakePrism
from OCP.GC import GC_MakeArcOfCircle
from OCP.GProp import GProp_GProps
from OCP.BRep import BRep_Tool
from OCP.BRepMesh import BRepMesh_IncrementalMesh
from OCP.STEPControl import STEPControl_AsIs, STEPControl_Writer
from OCP.TopAbs import TopAbs_FACE, TopAbs_REVERSED
from OCP.TopExp import TopExp_Explorer
from OCP.TopLoc import TopLoc_Location
from OCP.TopoDS import TopoDS
from OCP.gp import gp_Ax2, gp_Dir, gp_Pnt, gp_Trsf, gp_Vec

# ===================== 与 a2_shell.py 保持一致的接口参数 =====================
BOARD = (100.0, 65.0)
R_CORNER = 5.0
MARGIN = 3.0
WALL = 2.4
FLOOR = 2.0
BOARD_T = 1.6
BELOW_MAX = 5.60
GAP_BELOW = 1.0
STAND_H = BELOW_MAX + GAP_BELOW          # 6.60

STAND_D, HOLE_D = 7.0, 3.2
BOSS_D, BOSS_PILOT, BOSS_TAB_IN = 7.0, 2.7, 1.7
HOLE_LOCAL = [(6.00, 6.10), (94.00, 6.10), (6.00, 58.91)]   # 板坐标

W = BOARD[0] + 2 * (MARGIN + WALL)       # 110.8
H = BOARD[1] + 2 * (MARGIN + WALL)       # 75.8
OX = OY = MARGIN + WALL                  # 5.4

Z_FLOOR_TOP = -STAND_H                   # -6.60（板面 z=0 ⇒ 柱顶）
Z_BOTTOM = Z_FLOOR_TOP - FLOOR           # -8.60

R_OUT = R_CORNER + MARGIN + WALL         # 10.4
R_IN = R_CORNER + MARGIN                 # 8.0
CORNERS = [(BOSS_TAB_IN, BOSS_TAB_IN),
           (W - BOSS_TAB_IN, BOSS_TAB_IN),
           (BOSS_TAB_IN, H - BOSS_TAB_IN),
           (W - BOSS_TAB_IN, H - BOSS_TAB_IN)]

# 试印件只保留"浅盘"这一段（整壳的立墙不开，省料省时）
LIP_H = 2.0                              # 盘沿高出腔底 ⇒ 只用来定位板的四周


# ============================ 几何小工具 ============================
def translated(shape, dz, dx=0.0, dy=0.0):
    t = gp_Trsf()
    t.SetTranslation(gp_Vec(dx, dy, dz))
    return BRepBuilderAPI_Transform(shape, t, True).Shape()


def rounded_rect_prism(w, h, r, z0, z1):
    def pt(x, y):
        return gp_Pnt(x, y, 0)

    s45, c45 = math.sin(math.pi / 4), math.cos(math.pi / 4)
    ed = [
        BRepBuilderAPI_MakeEdge(pt(r, 0), pt(w - r, 0)).Edge(),
        BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(pt(w - r, 0), pt(w - r + r * s45, r - r * c45), pt(w, r)).Value()).Edge(),
        BRepBuilderAPI_MakeEdge(pt(w, r), pt(w, h - r)).Edge(),
        BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(pt(w, h - r), pt(w - r + r * s45, h - r + r * c45), pt(w - r, h)).Value()).Edge(),
        BRepBuilderAPI_MakeEdge(pt(w - r, h), pt(r, h)).Edge(),
        BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(pt(r, h), pt(r - r * s45, h - r + r * c45), pt(0, h - r)).Value()).Edge(),
        BRepBuilderAPI_MakeEdge(pt(0, h - r), pt(0, r)).Edge(),
        BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(pt(0, r), pt(r - r * s45, r - r * c45), pt(r, 0)).Value()).Edge(),
    ]
    mw = BRepBuilderAPI_MakeWire()
    for e in ed:
        mw.Add(e)
    face = BRepBuilderAPI_MakeFace(mw.Wire())
    prism = BRepPrimAPI_MakePrism(face.Face(), gp_Vec(0, 0, z1 - z0)).Shape()
    return translated(prism, z0)


def box(x0, y0, z0, x1, y1, z1):
    return BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, z0), gp_Pnt(x1, y1, z1)).Shape()


def cyl(x, y, z, r, h):
    return BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(x, y, z), gp_Dir(0, 0, 1)), r, h).Shape()


def cut(a, *bs):
    for b in bs:
        a = BRepAlgoAPI_Cut(a, b).Shape()
    return a


def fuse(a, *bs):
    for b in bs:
        a = BRepAlgoAPI_Fuse(a, b).Shape()
    return a


def vol(shape):
    p = GProp_GProps()
    BRepGProp.VolumeProperties_s(shape, p)
    return p.Mass()


def bbox(shape):
    """(xmin, ymin, zmin, xmax, ymax, zmax) —— 含外凸耳的真实外廓（走三角化，稳）。"""
    BRepMesh_IncrementalMesh(shape, 0.05, False, 0.3, True)
    xs, ys, zs = [], [], []
    ex = TopExp_Explorer(shape, TopAbs_FACE)
    while ex.More():
        f = TopoDS.Face(ex.Current())
        loc = TopLoc_Location()
        tri = BRep_Tool.Triangulation_s(f, loc)
        if tri is not None:
            trsf = loc.Transformation()
            for k in range(1, tri.NbNodes() + 1):
                p = tri.Node(k).Transformed(trsf)
                xs.append(p.X()); ys.append(p.Y()); zs.append(p.Z())
        ex.More() and ex.Next()
    return min(xs), min(ys), min(zs), max(xs), max(ys), max(zs)


def export_step(shape, path):
    w = STEPControl_Writer()
    w.Transfer(shape, STEPControl_AsIs)
    st = w.Write(path)
    print(f"    STEP → {os.path.basename(path)}  ({'OK' if str(st).endswith('RetDone') else st})")


def export_stl(shape, path, tol=0.05):
    """三角化 → STL（按 TopAbs_REVERSED 翻转朝向，否则体积为负）。"""
    import numpy as np
    import trimesh

    BRepMesh_IncrementalMesh(shape, tol, False, 0.3, True)
    verts, faces = [], []
    ex = TopExp_Explorer(shape, TopAbs_FACE)
    while ex.More():
        f = TopoDS.Face(ex.Current())
        loc = TopLoc_Location()
        tri = BRep_Tool.Triangulation_s(f, loc)
        if tri is not None:
            trsf = loc.Transformation()
            base = len(verts)
            rev = (f.Orientation() == TopAbs_REVERSED)
            for k in range(1, tri.NbNodes() + 1):
                p = tri.Node(k).Transformed(trsf)
                verts.append((p.X(), p.Y(), p.Z()))
            for k in range(1, tri.NbTriangles() + 1):
                a, b, c = tri.Triangle(k).Get()
                if rev:
                    b, c = c, b
                faces.append((base + a - 1, base + b - 1, base + c - 1))
        ex.More() and ex.Next()
    m = trimesh.Trimesh(vertices=np.array(verts), faces=np.array(faces), process=True)
    if m.volume < 0:
        m.invert()
    m.export(path)
    print(f"    STL  → {os.path.basename(path)}  {len(m.faces):,} 面  水密 {m.is_watertight}  "
          f"尺寸 {np.round(m.bounds[1] - m.bounds[0], 2)}  体积 {m.volume / 1000:.2f} cm³")
    return m


# ============================ 建模 ============================
def build():
    # ① 浅盘：外形 + 内腔（内腔只挖到 LIP_H 深，省料）
    solid = rounded_rect_prism(W, H, R_OUT, Z_BOTTOM, Z_FLOOR_TOP + LIP_H)
    inner = rounded_rect_prism(W - 2 * WALL, H - 2 * WALL, R_IN, Z_FLOOR_TOP, Z_FLOOR_TOP + LIP_H + 1)
    solid = cut(solid, translated(inner, 0, WALL, WALL))

    # ② 3 个主板安装柱（Φ7 / 高 6.6 / Φ3.2 贯通）
    solid = fuse(solid, *[cyl(OX + hx, OY + hy, Z_FLOOR_TOP, STAND_D / 2, STAND_H) for hx, hy in HOLE_LOCAL])
    solid = cut(solid, *[cyl(OX + hx, OY + hy, Z_BOTTOM - 1, HOLE_D / 2, STAND_H + FLOOR + 2)
                         for hx, hy in HOLE_LOCAL])

    # ③ 4 个 M3 外凸耳（Φ7 / Φ2.7 自攻预孔）：做到 z = +1.4，够拧 8 mm 牙
    tab_top = Z_FLOOR_TOP + STAND_H + BOARD_T + LIP_H - 0.2
    solid = fuse(solid, *[cyl(x, y, Z_FLOOR_TOP, BOSS_D / 2, tab_top - Z_FLOOR_TOP) for x, y in CORNERS])
    solid = cut(solid, *[cyl(x, y, Z_BOTTOM - 1, BOSS_PILOT / 2, tab_top - Z_BOTTOM + 2) for x, y in CORNERS])

    # ④ 第 4 安装孔让位（厂商原板 PCB1 的孔，改板已取消；壳底留 Φ8）
    solid = cut(solid, cyl(OX + 94.00, OY + 58.91, Z_BOTTOM - 1, 4.0, FLOOR + 2))
    return solid


def report(solid):
    xs = [OX + hx for hx, _ in HOLE_LOCAL]
    ys = [OY + hy for _, hy in HOLE_LOCAL]
    dx = max(xs) - min(xs)
    dy = max(ys) - min(ys)
    diag = math.hypot(dx, dy)
    tdx = CORNERS[1][0] - CORNERS[0][0]
    tdy = CORNERS[2][1] - CORNERS[0][1]
    tdiag = math.hypot(tdx, tdy)
    print("    安装柱（壳坐标 / mm）：" + "，".join(f"({x:.2f}, {y:.2f})" for x, y in zip(xs, ys)))
    print(f"    安装柱跨距：X {dx:.2f} × Y {dy:.2f}（对角 {diag:.2f}）—— 应为 88.00 / 52.81 / 102.63")
    print("    凸耳（壳坐标 / mm）：" + "，".join(f"({x:.2f}, {y:.2f})" for x, y in CORNERS))
    print(f"    凸耳跨距：X {tdx:.2f} × Y {tdy:.2f}（对角 {tdiag:.2f}）"
          f"—— 应为 {W - 2 * BOSS_TAB_IN:.2f} / {H - 2 * BOSS_TAB_IN:.2f} / "
          f"{math.hypot(W - 2 * BOSS_TAB_IN, H - 2 * BOSS_TAB_IN):.2f}")
    b = bbox(solid)
    print(f"    体积 {vol(solid) / 1000:.2f} cm³　壳体轮廓 {W:.1f} × {H:.1f}　"
          f"含凸耳外廓 {b[3] - b[0]:.2f} × {b[4] - b[1]:.2f} × {b[5] - b[2]:.2f} mm")
    return {
        "安装柱跨距": [round(dx, 2), round(dy, 2), round(diag, 2)],
        "凸耳跨距": [round(tdx, 2), round(tdy, 2), round(tdiag, 2)],
        "壳体轮廓": [W, H],
        "含凸耳外廓": [round(b[3] - b[0], 2), round(b[4] - b[1], 2), round(b[5] - b[2], 2)],
        "体积_cm3": round(vol(solid) / 1000, 2),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", default=os.environ.get(
        "A2_SHELL_COUPON_OUT",
        r"D:\260921Task_私有资料\正式作业提交\_A2审阅\外壳起点件"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    print("① 建孔位试印件（下壳底板段 + 3 安装柱 + 4 外凸耳）")
    solid = build()
    info = report(solid)

    base = os.path.join(a.out, "A2-05_孔位试印件")
    export_step(solid, base + ".step")
    export_stl(solid, base + ".stl")

    import json
    with open(base + "_参数.json", "w", encoding="utf-8") as f:
        json.dump(info, f, ensure_ascii=False, indent=2)
    print(f"② 完成 → {a.out}")
    print("   打印后检查：① 板落进内腔四周留 3.0（板 100×65 / 内腔 105.9×70.9）；"
          "② 铜柱穿过 3 个 Φ3.2 不别劲（跨距 88.00 × 52.81）；"
          "③ M3 自攻进 4 个 Φ2.7 不吃边（跨距 107.40 × 72.40）；"
          "④ 板坐在柱顶后，板底器件（最低 SW2 5.60）不得碰到腔底（柱高 6.6 ⇒ 余量 1.0）。")


if __name__ == "__main__":
    main()
