# -*- coding: utf-8 -*-
"""离线网格/STEP 工具：读模型 → 出信息、视图、零件图、接口校验（A2 用；不需要装 CAD）。

用法：
  python tools/mesh_tools.py info   <模型>
  python tools/mesh_tools.py render <模型> -o 视图.png [--size 2400]
  python tools/mesh_tools.py holes  <模型>                       # 检出竖直通孔（中心/直径/所在高度）
  python tools/mesh_tools.py check  <模型> [--clearance-mm 0.5]   # 与主板 100×65 的干涉/孔位对齐校验
  python tools/mesh_tools.py drawing <模型> -o 零件图.pdf          # 三视图 + 尺寸 + 孔表 + 图框标题栏

支持的输入：STL / OBJ / PLY / GLB / OFF / **STEP(.step/.stp)**（多实体 STEP 会保留零件层级）
"""
import argparse, math, os, sys, json
import numpy as np
import trimesh
from trimesh.proximity import ProximityQuery

# ------------------------------------------------------------------ 载入
def load_model(path):
    """返回 (parts:list[Trimesh], names:list[str])；STEP/多体文件保留分件。

    ★ 场景变换必须按 graph.nodes_geometry 逐节点取（早前按 geometry 字典顺序配 T 会错位，
      表现为"多数零件停在原点、中心坐标全是 (0,0)"）。
    """
    obj = trimesh.load(path, force="scene")
    parts, names = [], []
    if isinstance(obj, trimesh.Scene):
        for node in obj.graph.nodes_geometry:
            T, geom_name = obj.graph[node]
            geom = obj.geometry.get(geom_name)
            if not isinstance(geom, trimesh.Trimesh) or not len(geom.faces):
                continue
            part = geom.copy()
            if T is not None:
                part.apply_transform(T)
            parts.append(part)
            names.append(str(node or geom_name))
    elif isinstance(obj, trimesh.Trimesh):
        parts, names = [obj], ["part0"]
    if not parts:
        raise SystemExit("没读出网格（格式不支持？试导出 STL 或 STEP）")
    parts, k = fix_units(parts)
    if k != 1.0:
        print(f"（单位换算：×{k:g} ⇒ 毫米）")
    return parts, names

def merged(parts):
    return trimesh.util.concatenate(parts) if len(parts) > 1 else parts[0]

def find_board(parts):
    """按几何特征找出"主板本体"：XY 覆盖最大、Z 最薄的那块（不依赖零件名）。

    背景：STEP 读进来的节点名可能是 `=>[0:1:1:2]` 这类序号，靠名字找板不可靠。
    """
    best, best_score = None, -1
    for p in parts:
        s = p.bounds[1] - p.bounds[0]
        if s[2] > 3.0 or s[0] < 20 or s[1] < 20:
            continue
        score = s[0] * s[1] / max(s[2], 0.2)
        if score > best_score:
            best, best_score = p, score
    return best


def fix_units(parts):
    """STEP/STL 单位自适应：把"米/厘米"量级的模型换算成毫米（按最长边推断）。"""
    m = merged(parts)
    mx = float((m.bounds[1] - m.bounds[0]).max())
    if mx <= 0:
        return parts, 1.0
    if mx < 0.2:      k = 1000.0     # 米 → 毫米（STEP 常以 m 记）
    elif mx < 2.0:    k = 1000.0
    elif mx < 20.0:   k = 10.0       # 厘米 → 毫米
    else:             k = 1.0        # 已经是毫米
    if k != 1.0:
        for p in parts:
            p.apply_scale(k)
    return parts, k

# ------------------------------------------------------------------ 信息
def cmd_info(a):
    parts, names = load_model(a.file)
    m = merged(parts)
    bb = m.bounds; size = bb[1] - bb[0]
    print(f"文件：{os.path.basename(a.file)}")
    print(f"零件数：{len(parts)}　三角形：{len(m.faces):,}")
    print(f"外形尺寸（mm）：X {size[0]:.2f} × Y {size[1]:.2f} × Z {size[2]:.2f}")
    print(f"包围盒：X {bb[0][0]:.2f}~{bb[1][0]:.2f}　Y {bb[0][1]:.2f}~{bb[1][1]:.2f}　Z {bb[0][2]:.2f}~{bb[1][2]:.2f}")
    print(f"体积：{m.volume/1000:.2f} cm³　表面积：{m.area/100:.1f} cm²　水密(可打印)：{m.is_watertight}")
    if len(parts) > 1:
        print("分件：")
        for n, p in zip(names, parts):
            s = p.bounds[1] - p.bounds[0]
            print(f"  {n[:38]:<38} {s[0]:7.2f} × {s[1]:7.2f} × {s[2]:7.2f} mm  体积 {p.volume/1000:7.2f} cm³")
    holes, hz = detect_holes(m)
    print(f"孔轴方向：{"XYZ"[hz]} 轴（最薄方向）")
    if holes:
        print(f"检出竖直通孔 {len(holes)} 个（中心 X,Y / 直径）：")
        for h in holes[:24]:
            print(f"  ({h['u']:8.2f}, {h['v']:8.2f})  Φ{h['d']:6.2f} mm　厚度向 {h['t0']:.2f}~{h['t1']:.2f}")

# ------------------------------------------------------------------ 孔检出（水平截面里的内环）
def detect_holes(mesh, min_d=0.8, max_d=60.0, n_slices=40, axis=None):
    """检出"通孔"：自动判断孔轴方向（沿最短边为轴），再在该轴的垂直截面里找内环。"""
    ext = mesh.bounds[1] - mesh.bounds[0]
    if axis is None:
        axis = int(np.argmin(ext))          # 板类零件：最薄方向 = 孔轴方向
    m = mesh.copy()
    if axis != 2:                            # 旋转到"孔轴 = Z"再切
        if axis == 0:
            R = trimesh.transformations.rotation_matrix(math.radians(90), [0, 1, 0])
        else:
            R = trimesh.transformations.rotation_matrix(math.radians(-90), [1, 0, 0])
        m.apply_transform(R)
    bb = m.bounds; z0, z1 = bb[0][2], bb[1][2]
    holes, used = [], []
    for i in range(1, n_slices):
        z = z0 + (z1 - z0) * i / n_slices
        try:
            sec = m.section(plane_origin=[0, 0, z], plane_normal=[0, 0, 1])
        except Exception:
            continue
        if sec is None:
            continue
        try:
            p2, _ = sec.to_2D()
        except Exception:
            continue
        for poly in getattr(p2, "polygons_full", []) or []:
            for ring in getattr(poly, "interiors", []) or []:
                # ★ 内环是 LinearRing：area 恒为 0 ⇒ 用周长折算直径（近似圆）
                c = ring.centroid
                d = ring.length / math.pi
                if not (min_d <= d <= max_d):
                    continue
                if any(abs(c.x - u[0]) < 0.6 and abs(c.y - u[1]) < 0.6 and abs(d - u[2]) < 0.8 for u in used):
                    for h in holes:
                        if abs(h["u"] - c.x) < 0.6 and abs(h["v"] - c.y) < 0.6:
                            h["t1"] = max(h["t1"], z); h["t0"] = min(h["t0"], z)
                    continue
                used.append((c.x, c.y, d))
                holes.append({"u": round(c.x, 2), "v": round(c.y, 2), "d": round(d, 2), "t0": z, "t1": z})
    # 映射回原始坐标系
    out = []
    for h in holes:
        p = np.array([h["u"], h["v"], (h["t0"] + h["t1"]) / 2])
        if axis != 2:
            p = R[:3, :3].T @ p
        h["x"], h["y"], h["z"] = float(p[0]), float(p[1]), float(p[2])
        h["z0"], h["z1"] = round(h["t0"] - (0 if axis == 2 else 0), 2), round(h["t1"], 2)
        out.append(h)
    out.sort(key=lambda h: -h["d"])
    return out, axis

# ------------------------------------------------------------------ 视图渲染
def _proj(pts, view):
    """正交投影到 2D（返回 u,v）"""
    x, y, z = pts[:, 0], pts[:, 1], pts[:, 2]
    if view == "top":    return np.c_[x, -y]
    if view == "front":  return np.c_[x, z]
    if view == "right":  return np.c_[y, z]
    # iso
    a = math.radians(35.264); b = math.radians(45)
    u = (x - y) * math.cos(b)
    v = (x + y) * math.sin(b) * math.sin(a) + z * math.cos(a)
    return np.c_[u, v]

def render_views(mesh, out_png, size=2400, views=("iso", "front", "top", "right"), title=""):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection
    # 中文字体（否则标题/视图名会缺字变方框）
    for _f in ("SimHei", "Microsoft YaHei", "SimSun"):
        try:
            matplotlib.rcParams["font.sans-serif"] = [_f]; break
        except Exception:
            pass
    matplotlib.rcParams["axes.unicode_minus"] = False

    fig = plt.figure(figsize=(size / 200, size / 200), dpi=200)
    fig.patch.set_facecolor("white")
    if title:
        fig.suptitle(title, fontsize=int(size / 90), y=0.98)

    n = len(views)
    for k, v in enumerate(views):
        ax = fig.add_subplot(1, n, k + 1, projection="3d" if v == "iso" else None)
        if v == "iso":
            tri = mesh.vertices[mesh.faces]
            nrm = mesh.face_normals
            light = np.array([0.4, 0.5, 0.75]); light = light / np.linalg.norm(light)
            shade = np.clip(nrm @ light, 0, 1) * 0.65 + 0.35
            cols = np.c_[shade * 0.75, shade * 0.82, shade * 0.92, np.ones_like(shade)]
            pc = Poly3DCollection(tri, facecolors=cols, edgecolors=(0.15, 0.15, 0.2, 0.25), linewidths=0.08)
            ax.add_collection3d(pc)
            b = mesh.bounds
            ax.set_xlim(b[0][0], b[1][0]); ax.set_ylim(b[0][1], b[1][1]); ax.set_zlim(b[0][2], b[1][2])
            ax.set_box_aspect((b[1][0]-b[0][0], b[1][1]-b[0][1], b[1][2]-b[0][2]))
            ax.view_init(elev=32, azim=-58)
            ax.set_axis_off()
        else:
            uv = _proj(mesh.vertices, v)
            tri2 = uv[mesh.faces]
            from matplotlib.collections import PolyCollection
            nz = np.abs(mesh.face_normals[:, 2] if v == "top" else mesh.face_normals[:, 1] if v == "front" else mesh.face_normals[:, 0])
            shade = 0.45 + 0.5 * nz
            cols = np.c_[shade * 0.8, shade * 0.86, shade * 0.95, np.ones_like(shade)]
            ax.add_collection(PolyCollection(tri2[:, :, :2], facecolors=cols, edgecolors=(0.2, 0.2, 0.25, 0.3), linewidths=0.06))
            ax.autoscale_view(); ax.set_aspect("equal"); ax.set_axis_off()
        ax.set_title({"iso": "等轴测", "front": "主视图", "top": "俯视图", "right": "左视图"}[v], fontsize=int(size / 110))

    fig.tight_layout()
    fig.savefig(out_png, facecolor="white")
    print("已出图:", out_png)

# ------------------------------------------------------------------ 接口校验
def cmd_check(a):
    parts, _ = load_model(a.file)
    shell = merged(parts)
    # 由坐标表生成的三个安装孔（板左下角为原点）
    HOLES = [(6.00, 6.10), (94.00, 6.10), (6.00, 58.91)]
    print("=== 与主板的接口校验 ===")
    bb = shell.bounds; size = bb[1] - bb[0]
    print(f"壳内腔/外形：{size[0]:.2f} × {size[1]:.2f} × {size[2]:.2f} mm")
    print(f"板框要求：≥ 100.00 × 65.00（板）+ 每边 {a.clearance_mm} mm 间隙 ⇒ 需 ≥ "
          f"{100 + 2*a.clearance_mm:.1f} × {65 + 2*a.clearance_mm:.1f}")
    ok = size[0] >= 100 + 2*a.clearance_mm and size[1] >= 65 + 2*a.clearance_mm
    print("  ⇒", "✅ 尺寸够" if ok else "✗ 尺寸偏小（或你把原点放在了模型角上，需人工确认基准）")

    holes, _hax = detect_holes(shell, min_d=2.4, max_d=4.4)
    print(f"\n检出 Φ2.4~4.4 的孔 {len(holes)} 个；与 3 个安装孔位置比对：")
    for hx, hy in HOLES:
        best = None; bd = 1e9
        for h in holes:
            d = math.hypot(h["x"] - h["x"] if False else (h["x"] - hx), (h["y"] - hy))
            if d < bd: bd, best = d, h
        if best and bd <= 1.0:
            print(f"  ✅ ({hx:6.2f},{hy:6.2f}) ↔ 检出 ({best['x']:6.2f},{best['y']:6.2f}) Φ{best['d']:.2f}　偏差 {bd:.2f} mm")
        else:
            print(f"  ✗ ({hx:6.2f},{hy:6.2f}) 未找到对应柱/孔（最近 {bd if best else float('nan'):.2f} mm）")

    # 主板占位盒是否与壳干涉（用点采样，不依赖布尔）
    bx0, by0 = bb[0][0], bb[0][1]
    board = trimesh.creation.box(extents=[100.0, 65.0, 1.6])
    board.apply_translation([bx0 + 6 + 50 - 6, by0 + 6 + 32.5 - 6, bb[0][2] + 5])
    n = 4000
    pts = np.c_[np.random.uniform(board.bounds[0][0], board.bounds[1][0], n),
                np.random.uniform(board.bounds[0][1], board.bounds[1][1], n),
                np.random.uniform(board.bounds[0][2], board.bounds[1][2], n)]
    try:
        inside = shell.contains(pts)
        frac = float(np.mean(inside))
        print(f"\n主板占位盒采样 {n} 点：{int(inside.sum())} 点在壳体内（{frac*100:.2f}%）")
        print("  ⇒", "✅ 无明显干涉" if frac < 0.002 else "⚠ 有干涉/需人工确认（注意：模型坐标系与板基准可能不同）")
    except Exception as e:
        print("干涉采样失败：", e)

# ------------------------------------------------------------------ 零件图（三视图 + 尺寸 + 孔表 + 图框）
def cmd_drawing(a):
    parts, _ = load_model(a.file)
    m = merged(parts)
    bb = m.bounds; size = bb[1] - bb[0]
    views = ["front", "top", "right"]
    W, H = 2480, 1754          # A4 横放 @300dpi
    SCALE_FIT = 0.62
    from PIL import Image, ImageDraw, ImageFont
    F = lambda s, b=False: ImageFont.truetype(r"C:\Windows\Fonts\simhei.ttf" if b else r"C:\Windows\Fonts\simsun.ttc", s)
    im = Image.new("RGB", (W, H), "white"); d = ImageDraw.Draw(im)
    d.rectangle([24, 24, W-24, H-24], outline="#111", width=3)
    d.text((48, 40), f"零件图（自动生成）　{os.path.basename(a.file)}", font=F(34, True), fill="#111")
    d.text((48, 86), f"外形 {size[0]:.2f} × {size[1]:.2f} × {size[2]:.2f} mm　体积 {m.volume/1000:.2f} cm³　"
                     f"水密 {m.is_watertight}", font=F(24), fill="#333")
    # 三个视图，按比例摆放
    boxes = [(60, 150, 700, 700), (60, 760, 700, 1310), (760, 150, 1400, 700)]
    for (x0, y0, x1, y1), v in zip(boxes, views):
        uv = _proj(m.vertices, v)
        mn, mx = uv.min(axis=0), uv.max(axis=0)
        s = min((x1-x0-40)/max(mx[0]-mn[0], 1e-6), (y1-y0-40)/max(mx[1]-mn[1], 1e-6))
        P = np.c_[(uv[:, 0]-mn[0])*s + x0+20, (y1-20) - (uv[:, 1]-mn[1])*s]
        tris = P[m.faces]
        nz = np.abs(m.face_normals[:, {"front": 1, "top": 2, "right": 0}[v]])
        sh = 0.55 + 0.4*nz
        for t, c in zip(tris, sh):
            d.polygon([tuple(p) for p in t], fill=(int(200*c), int(210*c), int(230*c)), outline=(120, 130, 150))
        d.rectangle([x0, y0, x1, y1], outline="#bbb")
        d.text((x0+8, y1+6), {"front": "主视图", "top": "俯视图", "right": "左视图"}[v], font=F(24, True), fill="#111")
    # 孔表
    holes, _hax = detect_holes(m, min_d=1.5, max_d=40)
    d.text((1450, 150), f"检出孔/圆特征 {len(holes)} 个（Φ / 中心）", font=F(26, True), fill="#111")
    yy = 200
    for h in holes[:22]:
        d.text((1460, yy), f"Φ{h['d']:6.2f}   ({h['x']:7.2f}, {h['y']:7.2f})", font=F(22), fill="#333")
        yy += 30
    # 标题栏
    tb = [1140, 1560, W-40, H-40]
    d.rectangle(tb, outline="#111", width=2)
    rows = [("名称", os.path.splitext(os.path.basename(a.file))[0][:26]), ("材料", "PLA / PETG（按设计）"),
            ("比例", "见图"), ("单位", "mm"), ("设计", "A2（结构）"), ("日期", "2026-10-16")]
    rh = (tb[3]-tb[1])/len(rows)
    for i, (k, v) in enumerate(rows):
        y = tb[1] + i*rh
        d.line([tb[0], y, tb[2], y], fill="#111", width=1)
        d.text((tb[0]+10, y+8), f"{k}：{v}", font=F(22), fill="#111")
    out = a.out or (os.path.splitext(a.file)[0] + "_零件图.pdf")
    im.save(out, "PDF", resolution=300)
    print("已出零件图:", out, im.size)

# ------------------------------------------------------------------ CLI
def main(argv=None):
    ap = argparse.ArgumentParser(description="离线网格/STEP 工具（A2 用）")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info");    p.add_argument("file"); p.set_defaults(func=cmd_info)
    p = sub.add_parser("render");  p.add_argument("file"); p.add_argument("-o", "--out", required=True)
    p.add_argument("--size", type=int, default=2400); p.set_defaults(func=lambda a: render_views(merged(load_model(a.file)[0]), a.out, a.size))
    p = sub.add_parser("holes");   p.add_argument("file"); p.set_defaults(func=lambda a: cmd_info(a))
    p = sub.add_parser("check");   p.add_argument("file"); p.add_argument("--clearance-mm", type=float, default=0.5); p.set_defaults(func=cmd_check)
    p = sub.add_parser("drawing"); p.add_argument("file"); p.add_argument("-o", "--out"); p.set_defaults(func=cmd_drawing)
    args = ap.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()




