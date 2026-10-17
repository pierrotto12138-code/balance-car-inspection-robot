# -*- coding: utf-8 -*-
r"""A2 外壳生成器 v2 —— 在 v1（下壳 + 上壳毛坯）基础上补全"能装配"的结构。

v2 相对 v1 新增：
  ① **4 个 M3 角柱**（下壳）+ 上壳对应 **Φ3.4 通孔/沉台** ⇒ 上下壳可真正拧在一起
     （参考件里的 M3×11.25 / M3×24 / M3×60 就是这类固定螺钉；`MARGIN` 默认由 1.0 提到 3.0 以腾出角柱空间）
  ② **加强筋**：下壳底面 + 上壳顶面内侧各加纵/横筋（大平板防翘曲）
  ③ **走线孔**：下壳底面 Φ8 过线孔（电池/电机线进出）
  ④ **顶部通气栅格**：气体传感器正上方开栅格（形成对流，别把 MQ 闷死）
  ⑤ **可选电池仓**（`BATTERY_BAY`，默认关）与**可选显示屏窗**（`DISPLAY_WIN`，默认关，等实测）
  ⑥ 输出 **装配体 STEP**（上下壳一起），便于出装配图/爆炸图

参数集中在下方“参数”区，改完直接 `python tools\a2_shell.py`。
基准：原点 = 板左下角，板面 z = 0，元件面朝 +z。
"""
import json, math, os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from OCP.BRepPrimAPI import BRepPrimAPI_MakeBox, BRepPrimAPI_MakeCylinder, BRepPrimAPI_MakePrism
from OCP.BRepBuilderAPI import (BRepBuilderAPI_MakeWire, BRepBuilderAPI_MakeEdge,
                                BRepBuilderAPI_MakeFace, BRepBuilderAPI_Transform)
from OCP.GC import GC_MakeArcOfCircle
from OCP.gp import gp_Pnt, gp_Vec, gp_Ax2, gp_Dir, gp_Trsf
from OCP.BRepAlgoAPI import BRepAlgoAPI_Cut, BRepAlgoAPI_Fuse
from OCP.STEPControl import STEPControl_Writer, STEPControl_AsIs
from OCP.BRepGProp import BRepGProp
from OCP.GProp import GProp_GProps
from OCP.BRepMesh import BRepMesh_IncrementalMesh
from OCP.BRep import BRep_Tool
from OCP.TopExp import TopExp_Explorer
from OCP.TopAbs import TopAbs_FACE, TopAbs_REVERSED
from OCP.TopoDS import TopoDS
from OCP.TopLoc import TopLoc_Location
from mesh_tools import load_model, find_board

# ============================ 参数 ============================
BOARD = (100.0, 65.0)        # 板框（PCB2 3D STEP 实测 99.97 × 64.97）
R_CORNER = 5.0               # 板/壳四角圆角
MARGIN = 3.0                 # 板与壳内壁间隙（v1=1.0；提到 3.0 以放下 M3 角柱）
WALL = 2.4                   # 壁厚（参考件壳类件实测中位 2.38~2.98 mm ⇒ 取 2.4）
FLOOR = 2.0                  # 底板厚
BOARD_T = 1.6                # 板厚
BELOW_MAX = 5.60             # 板底最低器件（PCB2 3D STEP：SW2 引脚）
ABOVE_MAX = 13.00            # 板面最高器件（PCB2 3D STEP：J17 电源座）
GAP_BELOW, GAP_ABOVE = 1.0, 1.0
STAND_D, HOLE_D = 7.0, 3.2   # 安装柱外径 / 板上通孔
STAND_H = BELOW_MAX + GAP_BELOW
TOP_H = 22.0                 # 上壳内腔高（★ 控制板叠层实测后改）
LIP = 2.0                    # 下壳上沿高出板面

# 上下壳固定：**四角外凸耳**（腔内没有空间放 Φ7 柱 —— 板的 R5 圆角会干涉）
BOSS_D, BOSS_PILOT = 7.0, 2.7      # 凸耳外径 / M3 自攻预孔
SCREW_HOLE_TOP = 3.4               # 上壳对应通孔
BOSS_TAB_IN = 1.7                  # 凸耳中心自角点**向内**偏移（=Φ7 与板 R5 圆角外切且嵌进壳壁）

# 加强筋
RIB_T, RIB_H = 1.2, 1.5            # 筋厚 / 筋高
N_RIB_X, N_RIB_Y = 2, 1            # 下壳底面 纵/横 筋条数（0 = 不加）

# 走线孔
WIRE_HOLE_D = 8.0
WIRE_HOLE_AT = (72.0, 30.0)        # 板坐标（壳坐标 = +MARGIN+WALL）

# 顶部通气栅格（气体传感器正上方）
TOP_GRILLE_AT = [(80.70, 57.13), (94.91, 57.59)]
TOP_GRILLE_SLOTS, TOP_GRILLE_W, TOP_GRILLE_L = 5, 2.0, 14.0

# 可选件（默认关；需要时置 True 并给尺寸）
BATTERY_BAY = False
BATTERY = (94.5, 28.5, 13.2)       # 参考件 1900mAh 电池包络
DISPLAY_WIN = False
DISPLAY_WIN_RECT = (30.0, 12.0)    # (宽, 高) 前壁显示屏开窗，位置待实测

HOLE_LOCAL = [(6.00, 6.10), (94.00, 6.10), (6.00, 58.91)]
HOLE_EXTRA = (94.00, 58.91)

# 立墙开孔（板坐标；顶高默认值，实际按 PCB 3D 里器件真实高度自动覆盖）
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
GRILLE_AT = [(80.70, 57.13), (94.91, 57.59)]

OUT = os.environ.get("A2_SHELL_OUT",
                     r"D:\260921Task_私有资料\正式作业提交\_A2审阅\外壳起点件")
os.makedirs(OUT, exist_ok=True)

# ============================ 几何工具 ============================
W, H = BOARD[0] + 2 * (MARGIN + WALL), BOARD[1] + 2 * (MARGIN + WALL)
OX = OY = MARGIN + WALL


def translated(shape, dz, dx=0.0, dy=0.0):
    t = gp_Trsf(); t.SetTranslation(gp_Vec(dx, dy, dz))
    return BRepBuilderAPI_Transform(shape, t, True).Shape()


def rounded_rect_prism(w, h, r, z0, z1):
    def pt(x, y): return gp_Pnt(x, y, 0)
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


def centered_prism(w, h, r, z0, z1):
    return translated(rounded_rect_prism(w, h, r, 0, z1 - z0), z0, (W - w) / 2, (H - h) / 2)


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


def vol(s):
    p = GProp_GProps(); BRepGProp.VolumeProperties_s(s, p); return p.Mass()


def export_step(shape, path):
    w = STEPControl_Writer(); w.Transfer(shape, STEPControl_AsIs)
    st = w.Write(path)
    print(f"    STEP → {os.path.basename(path)}  ({'OK' if str(st).endswith('RetDone') else st})")


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
            rev = (f.Orientation() == TopAbs_REVERSED)
            for k in range(1, tri.NbNodes() + 1):
                p = tri.Node(k).Transformed(trsf); verts.append((p.X(), p.Y(), p.Z()))
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
          f"尺寸 {np.round(m.bounds[1]-m.bounds[0],2)}  体积 {m.volume/1000:.2f} cm³")


# ============================ PCB 3D 取数（开孔顶高） ============================
PCB_STEP = r"D:\260921Task\hardware\eda_export_20261016\3D_PCB2_2026-10-16.step"
_parts, _names = load_model(PCB_STEP)
_board = find_board(_parts)
_bx0, _by0, _bz0 = _board.bounds[0]
PARTS_Z = []
for _p in _parts:
    if _p is _board:
        continue
    c = (_p.bounds[0] + _p.bounds[1]) / 2
    PARTS_Z.append((c[0], c[1], _p.bounds[0][2] - _bz0, _p.bounds[1][2] - _bz0))
print(f"（PCB 3D：{len(_names)} 个零件，板 {_board.bounds[1][0]-_board.bounds[0][0]:.2f} × "
      f"{_board.bounds[1][1]-_board.bounds[0][1]:.2f} × {_board.bounds[1][2]-_board.bounds[0][2]:.2f} mm）")


def to_step(x, y):
    """我们的坐标（板左下角原点）→ PCB STEP 坐标：纯平移 (x-6.00, y-58.91)。"""
    return x - 6.00, y - 58.91


def opening_top(cx, cy, radius=10.0, default=3.0):
    sx, sy = to_step(cx, cy)
    zmax = None
    for (px, py, z0, z1) in PARTS_Z:
        if abs(px - sx) <= radius and abs(py - sy) <= radius and z1 > 2.0:
            zmax = z1 if zmax is None else max(zmax, z1)
    return round(max(default, (zmax + 1.0) if zmax is not None else default), 2)


# ============================ ① 下壳 ============================
z_floor_top = -STAND_H
z_bottom = z_floor_top - FLOOR
z_rim = BOARD_T + LIP

outer = rounded_rect_prism(W, H, R_CORNER + MARGIN + WALL, z_bottom, z_rim)
inner = centered_prism(W - 2 * WALL, H - 2 * WALL, R_CORNER + MARGIN, z_floor_top, z_rim + 1)
bottom = cut(outer, inner)
feat = {"下壳_外形": [W, H, round(z_rim - z_bottom, 2)], "壁厚": WALL, "底厚": FLOOR,
        "安装柱高": STAND_H, "板与内壁间隙": MARGIN}

# 板上 3 个安装柱 + 通孔
bottom = fuse(bottom, *[cyl(OX + hx, OY + hy, z_floor_top, STAND_D / 2, STAND_H) for hx, hy in HOLE_LOCAL])
bottom = cut(bottom, *[cyl(OX + hx, OY + hy, z_bottom - 1, HOLE_D / 2, STAND_H + FLOOR + 2) for hx, hy in HOLE_LOCAL])
# 第 4 孔（底盘柱）让位
bottom = cut(bottom, cyl(OX + HOLE_EXTRA[0], OY + HOLE_EXTRA[1], z_bottom - 1, 4.0, FLOOR + 2))

# ★ v2① 四角外凸耳（M3 自攻预孔 Φ2.7）
#   位置：自角点**向内 1.7 mm** ⇒ Φ7 圆与板的 R5 圆角**外切**（留 ~0.4 mm 间隙）、
#   同时又切进壳壁的圆角材料里 ⇒ 与壳一体、且不与板干涉
CORNERS = [(BOSS_TAB_IN, BOSS_TAB_IN),
           (W - BOSS_TAB_IN, BOSS_TAB_IN),
           (BOSS_TAB_IN, H - BOSS_TAB_IN),
           (W - BOSS_TAB_IN, H - BOSS_TAB_IN)]
bottom = fuse(bottom, *[cyl(x, y, z_floor_top, BOSS_D / 2, STAND_H + BOARD_T + LIP) for x, y in CORNERS])
bottom = cut(bottom, *[cyl(x, y, z_bottom - 1, BOSS_PILOT / 2, STAND_H + FLOOR + BOARD_T + LIP + 2) for x, y in CORNERS])
feat["四角外凸耳"] = {"壳坐标": CORNERS, "外径": BOSS_D, "M3预孔": BOSS_PILOT, "顶面z": round(z_rim, 2)}

# ★ v2② 底面加强筋
ribs = []
x0i, x1i = WALL, W - WALL
y0i, y1i = WALL, H - WALL
for k in range(N_RIB_X):
    x = x0i + (x1i - x0i) * (k + 1) / (N_RIB_X + 1)
    ribs.append(box(x - RIB_T / 2, y0i, z_floor_top, x + RIB_T / 2, y1i, z_floor_top + RIB_H))
for k in range(N_RIB_Y):
    y = y0i + (y1i - y0i) * (k + 1) / (N_RIB_Y + 1)
    ribs.append(box(x0i, y - RIB_T / 2, z_floor_top, x1i, y + RIB_T / 2, z_floor_top + RIB_H))
if ribs:
    bottom = fuse(bottom, *ribs)
feat["加强筋"] = {"纵向": N_RIB_X, "横向": N_RIB_Y, "厚": RIB_T, "高": RIB_H}

# ★ v2③ 走线孔
bottom = cut(bottom, cyl(OX + WIRE_HOLE_AT[0], OY + WIRE_HOLE_AT[1], z_bottom - 1,
                         WIRE_HOLE_D / 2, FLOOR + 2))
feat["走线孔"] = {"直径": WIRE_HOLE_D, "板坐标": WIRE_HOLE_AT}

# 立墙开孔（顶高按 PCB 3D 真实器件高度）
ops = []
for side, cx, cy, w, h, note in OPENINGS:
    X, Y = OX + cx, OY + cy
    h = opening_top(cx, cy, default=h)
    ops.append({"座": note, "边": side, "板坐标": [cx, cy], "宽": w, "顶高": h})
    if side == "L":
        c = box(-1, Y - w / 2, z_floor_top, MARGIN + WALL + 0.5, Y + w / 2, z_floor_top + h)
    elif side == "R":
        c = box(W - MARGIN - WALL - 0.5, Y - w / 2, z_floor_top, W + 1, Y + w / 2, z_floor_top + h)
    elif side == "T":
        c = box(X - w / 2, H - MARGIN - WALL - 0.5, z_floor_top, X + w / 2, H + 1, z_floor_top + h)
    else:
        c = box(X - w / 2, -1, z_floor_top, X + w / 2, MARGIN + WALL + 0.5, z_floor_top + h)
    bottom = cut(bottom, c)
feat["立墙开孔"] = ops

# 气体传感器栅格（立墙）
gcuts = []
for (cx, cy) in GRILLE_AT:
    for k in range(-2, 3):
        x = OX + cx + k * 5.0
        gcuts.append(box(x - 1.0, H - MARGIN - WALL - 0.5, z_floor_top + 1, x + 1.0, H + 1, z_floor_top + 6))
bottom = cut(bottom, *gcuts)

# ★ v2⑤ 可选电池仓（底面挡边）
if BATTERY_BAY:
    bw, bd, bh = BATTERY
    cx = W / 2
    y0 = H / 2 - (bd + 4) / 2
    t = 1.5
    walls = [box(cx - (bw + 4) / 2, y0, z_floor_top, cx - (bw + 4) / 2 + t, y0 + bd + 4, z_floor_top + bh),
             box(cx + (bw + 4) / 2 - t, y0, z_floor_top, cx + (bw + 4) / 2, y0 + bd + 4, z_floor_top + bh),
             box(cx - (bw + 4) / 2, y0, z_floor_top, cx + (bw + 4) / 2, y0 + t, z_floor_top + bh),
             box(cx - (bw + 4) / 2, y0 + bd + 4 - t, z_floor_top, cx + (bw + 4) / 2, y0 + bd + 4, z_floor_top + bh)]
    bottom = fuse(bottom, *walls)
    feat["电池仓"] = {"电池包络": BATTERY, "挡边高": bh}

print("① 下壳（v2）：")
print(f"    外形 {W:.1f} × {H:.1f} × {z_rim - z_bottom:.1f} mm　壁厚 {WALL}　底厚 {FLOOR}　"
      f"板间隙 {MARGIN}　安装柱高 {STAND_H:.2f}　M3 外凸耳 ×4（Φ{BOSS_D}/预孔Φ{BOSS_PILOT}）")
print(f"    体积 {vol(bottom)/1000:.2f} cm³")
export_step(bottom, os.path.join(OUT, "A2-01_下壳.step"))
export_stl(bottom, os.path.join(OUT, "A2-01_下壳.stl"))

# ============================ ② 上壳 ============================
z0 = z_rim - LIP
z1 = z0 + WALL + TOP_H
o2 = rounded_rect_prism(W, H, R_CORNER + MARGIN + WALL, z0, z1)
i2 = centered_prism(W - 2 * WALL, H - 2 * WALL, R_CORNER + MARGIN, z0 - 1, z1 - WALL)
top = cut(o2, i2)
# 与外凸耳对接的通孔（Φ3.4，从顶面往下贯穿到对插唇边）
top = cut(top, *[cyl(x, y, z0 - 1, SCREW_HOLE_TOP / 2, WALL + (z1 - z0) - WALL + 2) for x, y in CORNERS])
# 顶面内侧加强筋
tribs = []
zt = z1 - WALL
for k in range(N_RIB_X):
    x = WALL + (W - 2 * WALL) * (k + 1) / (N_RIB_X + 1)
    tribs.append(box(x - RIB_T / 2, WALL, zt - RIB_H, x + RIB_T / 2, H - WALL, zt))
for k in range(N_RIB_Y):
    y = WALL + (H - 2 * WALL) * (k + 1) / (N_RIB_Y + 1)
    tribs.append(box(WALL, y - RIB_T / 2, zt - RIB_H, W - WALL, y + RIB_T / 2, zt))
if tribs:
    top = fuse(top, *tribs)
# ★ v2④ 顶部通气栅格（传感器正上方）
tg = []
for (cx, cy) in TOP_GRILLE_AT:
    X, Y = OX + cx, OY + cy
    for k in range(TOP_GRILLE_SLOTS):
        y = Y - TOP_GRILLE_L / 2 + (k + 0.5) * TOP_GRILLE_L / TOP_GRILLE_SLOTS
        tg.append(box(X - TOP_GRILLE_W / 2, y - 0.8, zt - 1, X + TOP_GRILLE_W / 2, y + 0.8, z1 + 1))
top = cut(top, *tg)
feat["顶部通气栅格"] = {"位置": TOP_GRILLE_AT, "槽数": TOP_GRILLE_SLOTS,
                        "槽宽": TOP_GRILLE_W, "排布长": TOP_GRILLE_L}
# ★ v2⑤ 可选显示屏窗（前壁 = 上壳 T 侧）
if DISPLAY_WIN:
    ww, wh = DISPLAY_WIN_RECT
    top = cut(top, box(W / 2 - ww / 2, H - MARGIN - WALL - 0.5, z1 - WALL - 14 - wh,
                       W / 2 + ww / 2, H + 1, z1 - WALL - 14))
    feat["显示屏窗"] = {"宽高": DISPLAY_WIN_RECT, "位置": "上壳前壁（待实测复核）"}

print("② 上壳（v2）：")
print(f"    外形 {W:.1f} × {H:.1f} × {z1 - z0:.1f} mm　内腔高 {TOP_H:.1f}　"
      f"角柱通孔 Φ{SCREW_HOLE_TOP} ×4　顶部通气栅格 {TOP_GRILLE_SLOTS} 槽 ×{len(TOP_GRILLE_AT)} 处")
print(f"    体积 {vol(top)/1000:.2f} cm³")
export_step(top, os.path.join(OUT, "A2-02_上壳.step"))
export_stl(top, os.path.join(OUT, "A2-02_上壳.stl"))

# ============================ ③ 装配体 STEP ============================
asm = fuse(bottom, top)
export_step(asm, os.path.join(OUT, "A2-00_外壳装配.step"))
print(f"③ 装配体：体积 {vol(asm)/1000:.2f} cm³（不含板）")

json.dump(feat, open(os.path.join(OUT, "外壳_特征参数.json"), "w", encoding="utf-8"),
          ensure_ascii=False, indent=2)
print(f"\n特征参数 → 外壳_特征参数.json")


