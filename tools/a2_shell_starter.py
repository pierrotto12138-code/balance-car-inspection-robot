# -*- coding: utf-8 -*-
"""基于「我们自己的 PCB 3D STEP」生成 A2 外壳起点件（下壳 + 上壳毛坯）。

特点：
  · 基准 = 板左下角（与 A1/B/C 的坐标口径一致）；板面 z = 0，元件面朝 +z
  · 板框/安装孔/连接器位置全部取自 PCB2 的 3D STEP 与坐标表（不猜）
  · 安装柱高度按"板底最低器件 -5.60 mm"留 1 mm 余量
  · 输出 STEP（可编辑）+ STL（可打印）+ 视图；并做**干涉校验**（PCB 实体 ∩ 壳 体积应为 0）
"""
import os, sys, math
import numpy as np

sys.path.insert(0, r"D:\260921Task\tools")
from OCP.BRepPrimAPI import BRepPrimAPI_MakeBox, BRepPrimAPI_MakeCylinder, BRepPrimAPI_MakePrism
from OCP.BRepBuilderAPI import (BRepBuilderAPI_MakeWire, BRepBuilderAPI_MakeEdge, BRepBuilderAPI_MakeFace)
from OCP.GC import GC_MakeArcOfCircle
from OCP.gp import gp_Pnt, gp_Vec, gp_Ax2, gp_Dir
from OCP.BRepAlgoAPI import BRepAlgoAPI_Cut, BRepAlgoAPI_Fuse, BRepAlgoAPI_Common
from OCP.STEPControl import STEPControl_Writer, STEPControl_AsIs
from OCP.BRepGProp import BRepGProp
from OCP.GProp import GProp_GProps
from OCP.BRepMesh import BRepMesh_IncrementalMesh
from OCP.BRep import BRep_Tool
from OCP.TopExp import TopExp_Explorer
from OCP.TopAbs import TopAbs_FACE, TopAbs_REVERSED
from OCP.TopoDS import TopoDS
from OCP.TopLoc import TopLoc_Location

OUT = r"D:\260921Task_私有资料\正式作业提交\_A2审阅\外壳起点件"
os.makedirs(OUT, exist_ok=True)

# ---------------- 参数（A2 改这里即可）
BOARD = (100.0, 65.0)          # 板框（取自 PCB2 3D STEP：99.97 × 64.97）
R_CORNER = 5.0                 # 板/壳四角圆角
MARGIN = 1.0                   # 板与壳内壁的间隙（每边）
WALL = 2.0                     # 壳壁厚
FLOOR = 2.0                    # 底板厚
BOARD_T = 1.6                  # 板厚
BELOW_MAX = 5.60               # 板底最低器件（SW2 引脚，取自 PCB2 3D STEP）
ABOVE_MAX = 13.00              # 板面最高器件（J17，取自 PCB2 3D STEP）
GAP_BELOW = 1.0                # 板底器件与壳底的余量
GAP_ABOVE = 1.0                # 板面器件与上盖的余量
TOP_H = 22.0                   # 上盖内腔高度（★ 控制板叠层实测后改这里）
LIP = 2.0                      # 下壳上沿高出板面的高度（与上盖对插）

HOLE_LOCAL = [(6.00, 6.10), (94.00, 6.10), (6.00, 58.91)]      # 我们保留的 3 个 Φ3.2
HOLE_EXTRA = (94.00, 58.91)                                    # 厂商原板第 4 孔（底盘柱）→ 壳底做让位
STAND_D = 7.0                                                  # 安装柱外径
HOLE_D = 3.2                                                   # 通孔
STAND_H = BELOW_MAX + GAP_BELOW                                # 板底到壳底内表面

# 立墙开孔（板坐标 → 壳坐标 = +MARGIN+WALL 的偏移）：(边, 中心X, 中心Y, 宽, 高, 说明)
OPENINGS = [
    ("L",  6.00, 30.71, 11.0, 9.0,  "J17 12V 电源输入座"),
    ("T", 19.97, 60.97, 18.0, 9.0,  "CN1 电机A(6P)"),
    ("T", 38.00, 60.97, 18.0, 9.0,  "CN2 电机B(6P)"),
    ("T", 53.50, 61.96, 17.0, 11.0, "J5 超声波(4P)"),
    ("T", 80.70, 57.13, 14.0, 10.0, "J10 MQ-135(4P)"),
    ("T", 94.91, 57.59, 14.0, 10.0, "J13 MQ-2(4P)"),
    ("B", 59.34,  4.81, 18.0, 9.0,  "J7 蓝牙(6P)"),
    ("B", 74.58,  2.01, 12.0, 9.0,  "J18 蜂鸣器(3P)"),
    ("B", 85.25,  2.01, 12.0, 9.0,  "J9 DHT11(3P)"),
]
GRILLE_AT = [(80.70, 57.13), (94.91, 57.59)]                   # 气体传感器位置 → 立墙加栅格孔

W, H = BOARD[0] + 2 * (MARGIN + WALL), BOARD[1] + 2 * (MARGIN + WALL)   # 壳外形 106 × 71
OX = OY = MARGIN + WALL                                                  # 板左下角在壳坐标里的位置 = 3


def rounded_rect_prism(w, h, r, z0, z1):
    """带圆角的矩形柱（外壳/内腔都用它）"""
    def pt(x, y): return gp_Pnt(x, y, 0)
    edges = []
    edges.append(BRepBuilderAPI_MakeEdge(pt(r, 0), pt(w - r, 0)).Edge())
    edges.append(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(pt(w - r, 0), pt(w, r), pt(w, r)).Value()).Edge()
                 if False else BRepBuilderAPI_MakeEdge(
                     GC_MakeArcOfCircle(pt(w - r, 0), pt(w - r + r * math.sin(math.pi / 4),
                                                        r * (1 - math.cos(math.pi / 4))), pt(w, r)).Value()).Edge())
    edges.append(BRepBuilderAPI_MakeEdge(pt(w, r), pt(w, h - r)).Edge())
    edges.append(BRepBuilderAPI_MakeEdge(
        GC_MakeArcOfCircle(pt(w, h - r), pt(w - r + r * math.sin(math.pi / 4), h - r + r * math.cos(math.pi / 4)),
                           pt(w - r, h)).Value()).Edge())
    edges.append(BRepBuilderAPI_MakeEdge(pt(w - r, h), pt(r, h)).Edge())
    edges.append(BRepBuilderAPI_MakeEdge(
        GC_MakeArcOfCircle(pt(r, h), pt(r - r * math.sin(math.pi / 4), h - r + r * math.cos(math.pi / 4)),
                           pt(0, h - r)).Value()).Edge())
    edges.append(BRepBuilderAPI_MakeEdge(pt(0, h - r), pt(0, r)).Edge())
    edges.append(BRepBuilderAPI_MakeEdge(
        GC_MakeArcOfCircle(pt(0, r), pt(r - r * math.sin(math.pi / 4), r * (1 - math.cos(math.pi / 4))),
                           pt(r, 0)).Value()).Edge())
    mw = BRepBuilderAPI_MakeWire()
    for e in edges:
        mw.Add(e)
    face = BRepBuilderAPI_MakeFace(mw.Wire())
    prism = BRepPrimAPI_MakePrism(face.Face(), gp_Vec(0, 0, z1 - z0)).Shape()
    return translated(prism, z0), z0


def translated(shape, dz, dx=0.0, dy=0.0):
    from OCP.gp import gp_Trsf
    from OCP.BRepBuilderAPI import BRepBuilderAPI_Transform
    t = gp_Trsf(); t.SetTranslation(gp_Vec(dx, dy, dz))
    return BRepBuilderAPI_Transform(shape, t, True).Shape()


def centered_prism(w, h, r, z0, z1):
    """把 w×h 的圆角柱**居中**放在 W×H 的壳里（壁厚四边一致）"""
    shp, _ = rounded_rect_prism(w, h, r, 0, z1 - z0)   # ★ 该函数返回元组，要解包
    return translated(shp, z0, (W - w) / 2, (H - h) / 2)


def box(x0, y0, z0, x1, y1, z1):
    return BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, z0), gp_Pnt(x1, y1, z1)).Shape()


def cyl(x, y, z, r, h):
    return BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(x, y, z), gp_Dir(0, 0, 1)), r, h).Shape()


def vol(shape):
    p = GProp_GProps(); BRepGProp.VolumeProperties_s(shape, p); return p.Mass()


def export_step(shape, path):
    w = STEPControl_Writer(); w.Transfer(shape, STEPControl_AsIs)
    st = w.Write(path)
    print(f"  STEP → {os.path.basename(path)}  ({st})")


def export_stl(shape, path, tol=0.05):
    import trimesh
    BRepMesh_IncrementalMesh(shape, tol, False, 0.3, True)
    verts, faces = [], []
    ex = TopExp_Explorer(shape, TopAbs_FACE)
    while ex.More():
        f = TopoDS.Face(ex.Current()); loc = TopLoc_Location()
        tri = BRep_Tool.Triangulation_s(f, loc)
        if tri is not None:
            trsf = loc.Transformation(); base = len(verts)
            rev = (f.Orientation() == TopAbs_REVERSED)      # ★ 面朝向决定三角面绕向
            for k in range(1, tri.NbNodes() + 1):
                p = tri.Node(k).Transformed(trsf); verts.append((p.X(), p.Y(), p.Z()))
            for k in range(1, tri.NbTriangles() + 1):
                a, b, c = tri.Triangle(k).Get()
                if rev:
                    b, c = c, b
                faces.append((base + a - 1, base + b - 1, base + c - 1))
        ex.Next()
    m = trimesh.Trimesh(vertices=np.array(verts), faces=np.array(faces), process=True)
    if m.volume < 0:                     # OCP 三角面绕向与 trimesh 相反 ⇒ 翻转
        m.invert()
    m.export(path)
    print(f"  STL → {os.path.basename(path)}  {len(m.faces):,} 面  水密 {m.is_watertight}  "
          f"尺寸 {np.round(m.bounds[1]-m.bounds[0],2)}  体积 {m.volume/1000:.2f} cm³")


# ---------------- 读 PCB 3D：拿到每个器件的真实高度（板局部坐标）
from mesh_tools import load_model, find_board   # noqa: E402
PCB_STEP = r"D:\260921Task\hardware\eda_export_20261016\3D_PCB2_2026-10-16.step"
_parts, _names = load_model(PCB_STEP)
_board = find_board(_parts)
_bx0, _by0, _bz0 = _board.bounds[0]
_btop = _board.bounds[1][2] - _bz0                      # 板厚（≈1.6）
PARTS_Z = []            # (中心x, 中心y, z_min, z_max) —— **STEP 原始坐标**（不是板局部）
for _p in _parts:
    if _p is _board:
        continue
    c = (_p.bounds[0] + _p.bounds[1]) / 2
    PARTS_Z.append((c[0], c[1], _p.bounds[0][2] - _bz0, _p.bounds[1][2] - _bz0))


def to_step(x, y):
    """我们的坐标表（板左下角原点、y 向上）→ PCB STEP 坐标。
    依据：板上 3 个安装孔在 STEP 里位于 (0,0)/(0,-52.81)/(88,-52.81)，
    对应我们的 (6.00,6.10)/(6.00,58.91)/(94.00,6.10) ⇒ x-6.00, -(y-6.10)。"""
    return x - 6.00, y - 58.91


def opening_top(cx, cy, radius=10.0, default=3.0):
    """该位置附近器件的最高点 +1 mm ⇒ 开孔顶高（保证连接器本体能伸出去）"""
    sx, sy = to_step(cx, cy)
    zmax = None
    for (px, py, z0, z1) in PARTS_Z:
        if abs(px - sx) <= radius and abs(py - sy) <= radius:
            zmax = z1 if zmax is None else max(zmax, z1)
    return round(max(default, (zmax + 1.0) if zmax is not None else default), 2)


# ---------------- ① 下壳
z_floor_top = -STAND_H                       # 壳底内表面（板底 z=0 往下 STAND_H）
z_bottom = z_floor_top - FLOOR               # 壳外底
z_rim = BOARD_T + LIP                        # 下壳上沿（高于板面 LIP）
outer, _ = rounded_rect_prism(W, H, R_CORNER + MARGIN + WALL, z_bottom, z_rim)
inner = centered_prism(W - 2 * WALL, H - 2 * WALL, R_CORNER + MARGIN, z_floor_top, z_rim + 1)
shell = BRepAlgoAPI_Cut(outer, inner).Shape()

# 安装柱（3 个，通孔从底穿）
for (hx, hy) in HOLE_LOCAL:
    boss = cyl(OX + hx, OY + hy, z_floor_top, STAND_D / 2, STAND_H)
    shell = BRepAlgoAPI_Fuse(shell, boss).Shape()
for (hx, hy) in HOLE_LOCAL:
    hole = cyl(OX + hx, OY + hy, z_bottom - 1, HOLE_D / 2, STAND_H + FLOOR + 2)
    shell = BRepAlgoAPI_Cut(shell, hole).Shape()
# 第 4 个孔位（底盘柱）→ 壳底让位孔 Φ8
shell = BRepAlgoAPI_Cut(shell, cyl(OX + HOLE_EXTRA[0], OY + HOLE_EXTRA[1], z_bottom - 1, 4.0, FLOOR + 2)).Shape()

# 立墙开孔
for side, cx, cy, w, h, note in OPENINGS:
    X, Y = OX + cx, OY + cy
    h = opening_top(cx, cy, default=h)          # ★ 顶高按器件真实高度定
    print(f"   开孔 {note:<22} 顶高 {h:5.2f} mm（含出线余量）")
    if side == "L":   cut = box(-1, Y - w / 2, z_floor_top - 0.0, MARGIN + WALL + 0.5, Y + w / 2, z_floor_top + h)
    elif side == "R": cut = box(W - MARGIN - WALL - 0.5, Y - w / 2, z_floor_top, W + 1, Y + w / 2, z_floor_top + h)
    elif side == "T": cut = box(X - w / 2, H - MARGIN - WALL - 0.5, z_floor_top, X + w / 2, H + 1, z_floor_top + h)
    else:             cut = box(X - w / 2, -1, z_floor_top, X + w / 2, MARGIN + WALL + 0.5, z_floor_top + h)
    shell = BRepAlgoAPI_Cut(shell, cut).Shape()
# 气体传感器：立墙加栅格孔（Φ3 一列 5 个）
for (cx, cy) in GRILLE_AT:
    for k in range(-2, 3):
        x = OX + cx + k * 5.0
        cut = box(x - 1.0, H - MARGIN - WALL - 0.5, z_floor_top + 1, x + 1.0, H + 1, z_floor_top + 6)
        shell = BRepAlgoAPI_Cut(shell, cut).Shape()

print("① 下壳：")
print(f"  外形 {W:.1f} × {H:.1f} × {z_rim - z_bottom:.1f} mm　底厚 {FLOOR}　壁厚 {WALL}　"
      f"安装柱高 {STAND_H:.2f}（板底最低器件 {BELOW_MAX} + 余量 {GAP_BELOW}）")
print(f"  板面最高器件 {ABOVE_MAX} mm ⇒ 上盖内腔至少 {ABOVE_MAX + GAP_ABOVE:.1f} mm")
print(f"  体积 {vol(shell)/1000:.2f} cm³")
export_step(shell, os.path.join(OUT, "A2-01_下壳_起点件.step"))
export_stl(shell, os.path.join(OUT, "A2-01_下壳_起点件.stl"))

# ---------------- ② 上壳毛坯（内腔高 = 参数；开窗等 A2 按人机测量补）
z0 = z_rim - LIP          # 与下壳搭接
z1 = z0 + WALL + TOP_H    # 上盖外顶
o2, _ = rounded_rect_prism(W, H, R_CORNER + MARGIN + WALL, z0, z1)
i2 = centered_prism(W - 2 * WALL, H - 2 * WALL, R_CORNER + MARGIN, z0 - 1, z1 - WALL)
top = BRepAlgoAPI_Cut(o2, i2).Shape()
print("② 上壳毛坯：")
print(f"  外形 {W:.1f} × {H:.1f} × {z1 - z0:.1f} mm　内腔高 {TOP_H:.1f}（★ 控制板叠层实测后改）")
print(f"  体积 {vol(top)/1000:.2f} cm³")
export_step(top, os.path.join(OUT, "A2-02_上壳毛坯_起点件.step"))
export_stl(top, os.path.join(OUT, "A2-02_上壳毛坯_起点件.stl"))

# ---------------- ③ 与 PCB 实体的干涉校验
import trimesh
from mesh_tools import load_model, merged
pcb = merged(load_model(r"D:\260921Task\hardware\eda_export_20261016\3D_PCB2_2026-10-16.step")[0])
print(f"\n③ 干涉校验：PCB 实体 {len(pcb.faces):,} 面")
import trimesh as _tm
from mesh_tools import load_model as _lm, merged as _mg
# 用 Board 分件定位：把板左下角对齐到壳坐标 (OX, OY, 0)
parts, names = _lm(r"D:\260921Task\hardware\eda_export_20261016\3D_PCB2_2026-10-16.step")
board_mesh = None
for n, p in zip(names, parts):
    if n.startswith("Board"):
        board_mesh = p; break
if board_mesh is not None:
    b0 = board_mesh.bounds[0]
    shift = np.array([OX, OY, 0.0]) - b0
    pcb2 = pcb.copy(); pcb2.apply_translation(shift)
    shell_mesh = _tm.load(os.path.join(OUT, "A2-01_下壳_起点件.stl"))
    try:
        inter = pcb2.intersection(shell_mesh)
        v = inter.volume / 1000 if inter is not None and len(getattr(inter, 'faces', [])) else 0.0
        print(f"   PCB ∩ 下壳 = {v:.4f} cm³  ⇒ " + ("✅ 无干涉" if abs(v) < 0.01 else "⚠ 有干涉，需看是不是安装柱/壁面"))
    except Exception as e:
        print("   布尔求交失败：", e)
    print(f"   板框 {board_mesh.bounds[1][0]-board_mesh.bounds[0][0]:.2f} × "
          f"{board_mesh.bounds[1][1]-board_mesh.bounds[0][1]:.2f} mm（应 ≈ {BOARD[0]} × {BOARD[1]}）")




