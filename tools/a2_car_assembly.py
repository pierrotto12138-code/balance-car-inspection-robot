# -*- coding: utf-8 -*-
"""整车装配（做"看起来是完整的车"）：参考电机 + 参数化底盘/轮子 + M3 铜柱 + 我们的板与壳。

布局（壳坐标：板左下角 = (3,3)，板面 z = 0，元件面 +z）：
  壳底 z = -8.6 ── M3 铜柱 25 mm ── 底盘板上表面 z = -33.6 ── 底盘板厚 4
  轮心 z = -35.6（⌀67 ⇒ 轮底 = -69.1 = 地面）　轮在 X 两端，轴向沿 Y
输出：<OUT>\\A2-10_整车.stl / .png（四视图）/ .pdf
"""
import os, sys, math, json
import numpy as np
import trimesh

sys.path.insert(0, r"D:\260921Task\tools")
from OCP.BRepPrimAPI import BRepPrimAPI_MakeCylinder, BRepPrimAPI_MakeBox
from OCP.gp import gp_Pnt, gp_Ax2, gp_Dir
from OCP.BRepAlgoAPI import BRepAlgoAPI_Cut, BRepAlgoAPI_Fuse
from OCP.BRepMesh import BRepMesh_IncrementalMesh
from OCP.BRep import BRep_Tool
from OCP.TopExp import TopExp_Explorer
from OCP.TopAbs import TopAbs_FACE, TopAbs_REVERSED
from OCP.TopoDS import TopoDS
from OCP.TopLoc import TopLoc_Location
from mesh_tools import load_model, merged, find_board, render_views

OUT = r"D:\260921Task_私有资料\正式作业提交\_A2审阅\外壳起点件"
REF = r"D:\260921Task_私有资料\作业补充\A2\小车_STEP"
os.makedirs(OUT, exist_ok=True)

# ---------------- 关键参数（与 a2_shell.py 保持一致）
MARGIN, WALL = 3.0, 2.4
W, H = 100 + 2 * (MARGIN + WALL), 65 + 2 * (MARGIN + WALL)      # 110.8 × 75.8
OX = OY = MARGIN + WALL                                          # 板左下角在壳坐标 = 5.4
BELOW_MAX, GAP = 5.60, 1.0
STAND_H = BELOW_MAX + GAP                                        # 6.6
FLOOR = 2.0
Z_BOTTOM = -(STAND_H + FLOOR)                                    # -8.6
STANDOFF_H = 25.0                                                # ★ 铜柱
Z_CHASSIS_TOP = Z_BOTTOM - STANDOFF_H                            # -33.6
CHASSIS_T = 4.0
WHEEL_D, WHEEL_W = 67.0, 26.0
Z_WHEEL = Z_CHASSIS_TOP - CHASSIS_T / 2                          # 轮心 ≈ 底盘板中面
CHASSIS_L, CHASSIS_Wd = 110.0, 116.0       # X 左右（夹在两轮之间）/ Y 前后（承托电池）
CX, CY = OX + 100 / 2, OY + 65 / 2                               # 板中心（壳坐标）

# ---------------- OCP → trimesh
def to_mesh(shape, tol=0.15):
    BRepMesh_IncrementalMesh(shape, tol, False, 0.35, True)
    v, f = [], []
    ex = TopExp_Explorer(shape, TopAbs_FACE)
    while ex.More():
        face = TopoDS.Face(ex.Current()); loc = TopLoc_Location()
        tri = BRep_Tool.Triangulation_s(face, loc)
        if tri is not None:
            trsf = loc.Transformation(); base = len(v)
            rev = (face.Orientation() == TopAbs_REVERSED)
            for k in range(1, tri.NbNodes() + 1):
                p = tri.Node(k).Transformed(trsf); v.append((p.X(), p.Y(), p.Z()))
            for k in range(1, tri.NbTriangles() + 1):
                a, b, c = tri.Triangle(k).Get()
                if rev:
                    b, c = c, b
                f.append((base + a - 1, base + b - 1, base + c - 1))
        ex.Next()
    m = trimesh.Trimesh(vertices=np.array(v), faces=np.array(f), process=True)
    if m.volume < 0:
        m.invert()
    return m


def cyl(shape, x, y, z, r, h):
    return BRepAlgoAPI_Fuse(shape, BRepPrimAPI_MakeCylinder(
        gp_Ax2(gp_Pnt(x, y, z), gp_Dir(0, 0, 1)), r, h).Shape()).Shape()


def cylcut(shape, x, y, z, r, h):
    return BRepAlgoAPI_Cut(shape, BRepPrimAPI_MakeCylinder(
        gp_Ax2(gp_Pnt(x, y, z), gp_Dir(0, 0, 1)), r, h).Shape()).Shape()


# ---------------- ① 底盘板（参数化，带减重孔）
chassis = BRepPrimAPI_MakeBox(gp_Pnt(CX - CHASSIS_L / 2, CY - CHASSIS_Wd / 2, Z_CHASSIS_TOP - CHASSIS_T),
                              gp_Pnt(CX + CHASSIS_L / 2, CY + CHASSIS_Wd / 2, Z_CHASSIS_TOP)).Shape()
for i in range(5):
    x = CX - CHASSIS_L / 2 + 20 + i * 30
    for y in (CY - 22, CY + 22):
        chassis = cylcut(chassis, x, y, Z_CHASSIS_TOP - CHASSIS_T - 1, 6.0, CHASSIS_T + 2)
# 电机安装凸台（左右两侧 = ±X，靠近轮子；电机轴沿 X）
for sx in (-1, 1):
    xc = CX + sx * (CHASSIS_L / 2 - 14)
    chassis = BRepAlgoAPI_Fuse(chassis, BRepPrimAPI_MakeBox(
        gp_Pnt(xc - 14, CY - 26, Z_CHASSIS_TOP), gp_Pnt(xc + 14, CY + 26, Z_CHASSIS_TOP + 6)).Shape()).Shape()
chassis_m = to_mesh(chassis)
print(f"底盘板：{np.round(chassis_m.bounds[1]-chassis_m.bounds[0],1)}  z {chassis_m.bounds[0][2]:.1f}~{chassis_m.bounds[1][2]:.1f}")

# ---------------- ② 轮子（⌀67 × 26，带 6 个减重孔 + 轮毂）
def make_wheel(xc, yc):
    w = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(xc - WHEEL_W / 2, yc, Z_WHEEL), gp_Dir(1, 0, 0)),
                                 WHEEL_D / 2, WHEEL_W)
    shp = w.Shape()
    # 轮毂凸台（同轴，略宽）
    hub = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(xc - (WHEEL_W + 8) / 2, yc, Z_WHEEL), gp_Dir(1, 0, 0)),
                                   9.0, WHEEL_W + 8)
    shp = BRepAlgoAPI_Fuse(shp, hub.Shape()).Shape()
    # 减重孔（沿 Y 打穿）
    for k in range(6):
        a = k * math.pi / 3
        hy = yc + 19 * math.cos(a); hz = Z_WHEEL + 19 * math.sin(a)
        hole = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(xc - 80, hy, hz), gp_Dir(1, 0, 0)), 5.0, 200).Shape()
        shp = BRepAlgoAPI_Cut(shp, hole).Shape()
    # 中心轴孔
    shp = BRepAlgoAPI_Cut(shp, BRepPrimAPI_MakeCylinder(
        gp_Ax2(gp_Pnt(xc - 80, yc, Z_WHEEL), gp_Dir(1, 0, 0)), 4.0, 200).Shape()).Shape()
    return to_mesh(shp, 0.12)


XW = [(-(WHEEL_W / 2) - 0.5), (W + WHEEL_W / 2 + 0.5)]    # 左、右轮中心 x（装在窄侧，贴壳两侧外）
wheels = [make_wheel(xc, CY) for xc in XW]
for w, xc in zip(wheels, XW):
    b = w.bounds
    print(f"轮子 @x={xc:6.1f}（窄侧）：直径 {b[1][1]-b[0][1]:.1f} × {b[1][2]-b[0][2]:.1f}、宽 {b[1][0]-b[0][0]:.1f}")

# ---------------- ③ M3 铜柱 ×4（25 mm，Φ6 六角柱的圆柱近似）
stand = BRepAlgoAPI_Fuse(
    BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 3.0, STANDOFF_H).Shape(),
    BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 5.5, 3.0).Shape()).Shape()
stand_m = to_mesh(stand, 0.1)
BOSSP = 1.7
CORNER = [(BOSSP, BOSSP), (W - BOSSP, BOSSP), (BOSSP, H - BOSSP), (W - BOSSP, H - BOSSP)]
stands = []
for (x, y) in CORNER:
    s = stand_m.copy()
    s.apply_translation([x, y, Z_CHASSIS_TOP])
    stands.append(s)
print(f"M3 铜柱 ×4：高 {STANDOFF_H} mm，位于 {CORNER}")

# ---------------- ④ 参考件：电机 ×2 / 电池
def ref_mesh(fn):
    p = os.path.join(REF, fn)
    if not os.path.exists(p):
        return None
    t0 = __import__("time").time()
    parts, names = load_model(p)
    m = merged(parts)
    print(f"  读参考件 {fn[:22]} 用时 {__import__('time').time()-t0:.1f}s  尺寸 {np.round(m.bounds[1]-m.bounds[0],1)}")
    return m

mot = ref_mesh("____________.stp.step")
batt = ref_mesh("1900mah____.stp.step")

assemblies = [("底盘", chassis_m)]
for w in wheels:
    assemblies.append(("轮", w))
for s in stands:
    assemblies.append(("铜柱", s))

if mot is not None:
    mb = mot.bounds
    mc = (mb[0] + mb[1]) / 2
    for sx in (-1, 1):
        m2 = mot.copy()
        m2.apply_translation(-mc)                     # 中心到原点
        # 电机轴线沿 X（与轮轴同向），坐在底盘板左右凸台上、轮内侧
        xc = CX + sx * (CHASSIS_L / 2 - 16)
        m2.apply_translation([xc, CY, Z_CHASSIS_TOP + 6 + (mb[1][2] - mb[0][2]) / 2])
        assemblies.append(("电机", m2))
        print(f"  电机 @x={xc:6.1f} z≈{Z_CHASSIS_TOP + 6:.1f}（轴线沿 X）")

if batt is not None:
    bb = batt.bounds
    bc = (bb[0] + bb[1]) / 2
    b2 = batt.copy(); b2.apply_translation(-bc)
    b2.apply_transform(trimesh.transformations.rotation_matrix(math.pi / 2, [0, 0, 1]))  # 长边转到 Y（前进方向）
    bb2 = b2.bounds
    b2.apply_translation([CX - (bb2[0][0] + bb2[1][0]) / 2, CY - (bb2[0][1] + bb2[1][1]) / 2,
                          Z_CHASSIS_TOP + 3 - bb2[0][2]])
    assemblies.append(("电池", b2))

# ---------------- ⑤ 我们的板 + 壳
pcb = trimesh.load(os.path.join(OUT, "A2-03_主控板装配.stl"))
bot = trimesh.load(os.path.join(OUT, "A2-01_下壳.stl"))
top = trimesh.load(os.path.join(OUT, "A2-02_上壳.stl"))
for name, m in (("主控板", pcb), ("下壳", bot), ("上壳", top)):
    assemblies.append((name, m))

whole = trimesh.util.concatenate([m for _, m in assemblies])
stl = os.path.join(OUT, "A2-10_整车.stl")
whole.export(stl)
b = whole.bounds
print(f"\n整车：{np.round(b[1]-b[0],1)}　X {b[0][0]:.1f}~{b[1][0]:.1f}  Y {b[0][1]:.1f}~{b[1][1]:.1f}  "
      f"Z {b[0][2]:.1f}~{b[1][2]:.1f}　→ {os.path.basename(stl)}")

render_views(whole, os.path.join(OUT, "A2-10_整车_四视图.png"), size=2400,
             views=("iso", "front", "top", "right"), title="整车装配（轮 Φ67 / M3 铜柱 25 / 本组电控与外壳）")

json.dump({"零件": [n for n, _ in assemblies],
           "地面z": round(float(b[0][2]), 2), "板面到地面": round(float(-b[0][2]), 2),
           "铜柱高": STANDOFF_H, "轮径": WHEEL_D, "底盘厚": CHASSIS_T},
          open(os.path.join(OUT, "A2-10_整车_参数.json"), "w", encoding="utf-8"),
          ensure_ascii=False, indent=2)
print("已出：A2-10_整车.stl / A2-10_整车_四视图.png / A2-10_整车_参数.json")


