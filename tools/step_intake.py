# -*- coding: utf-8 -*-
r"""STEP/IGES 到手后的「体检 + 对照」入口（一条命令出全部结论）。

用法：
  python tools/step_intake.py <模型> [-o 输出目录] [--name 前缀]

输出：
  · 控制台：零件清单（尺寸/体积/水密）、壳体类壁厚、全部孔位、与**我们板**（100×65、3×Φ3.2、跨距 88×52.81）的对照
  · <输出目录>\<前缀>_四视图.png     ：等轴测 + 三视图
  · <输出目录>\<前缀>_摘要.json      ：机器可读的结论（便于后续脚本直接吃）
"""
import argparse, json, math, os, sys
import numpy as np
import trimesh

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mesh_tools import load_model, merged, find_board, detect_holes, render_views

BOARD_W, BOARD_H = 100.0, 65.0
BOARD_HOLES = [(6.00, 6.10), (94.00, 6.10), (6.00, 58.91)]
SPANS = (88.00, 52.81, round(math.hypot(88.00, 52.81), 2))


def wall_thickness(mesh, n=600, max_d=20.0):
    r"""壁厚估计：表面采样点沿内法向打射线，取首个非自身交点的距离中位数。"""
    try:
        pts, fid = trimesh.sample.sample_surface(mesh, n)
        nrm = mesh.face_normals[fid]
        origins = pts - nrm * 0.02
        dirs = -nrm
        loc, idx_ray, _ = mesh.ray.intersects_location(origins, dirs, multiple_hits=False)
        if len(loc) == 0:
            return None
        d = np.linalg.norm(loc - origins[idx_ray], axis=1)
        d = d[(d > 0.05) & (d < max_d)]
        return float(np.median(d)) if len(d) else None
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("-o", "--out", default=None)
    ap.add_argument("--name", default=None)
    a = ap.parse_args()

    stem = a.name or os.path.splitext(os.path.basename(a.file))[0][:24]
    outdir = a.out or os.path.join(os.path.dirname(os.path.abspath(a.file)), "intake_" + stem)
    os.makedirs(outdir, exist_ok=True)

    parts, names = load_model(a.file)
    m = merged(parts)
    B0, B1 = m.bounds
    size = B1 - B0
    rep = {"file": os.path.basename(a.file), "parts": len(parts),
           "size_mm": [round(float(v), 2) for v in size],
           "triangles": int(len(m.faces)), "watertight_overall": bool(m.is_watertight),
           "volume_cm3": round(float(m.volume) / 1000, 2)}

    print("=" * 78)
    print(f"文件：{rep['file']}　零件 {rep['parts']} 个　三角面 {rep['triangles']:,}")
    print(f"总体尺寸：{size[0]:.2f} × {size[1]:.2f} × {size[2]:.2f} mm　"
          f"整体水密 {m.is_watertight}　体积 {rep['volume_cm3']} cm³")
    if not m.is_watertight and m.volume < 0:
        print("⚠ 整体不水密且体积为负 ⇒ 很可能是**曲面模型**（IGES 未导实体）：只能量尺寸/看结构，"
              "不能打印、不能布尔校验。请让对方**导出实体（solid）STEP**。")

    # 分件清单：按体积/尺寸排序
    print("\n【分件清单】（按体积降序，前 20）")
    rows = []
    for p in parts:
        s = p.bounds[1] - p.bounds[0]
        rows.append({"size": [round(float(v), 2) for v in s], "vol": round(float(p.volume) / 1000, 3),
                     "watertight": bool(p.is_watertight)})
    for r in sorted(rows, key=lambda r: -abs(r["vol"]))[:20]:
        print(f"   {r['size'][0]:8.2f} × {r['size'][1]:7.2f} × {r['size'][2]:7.2f} mm　"
              f"体积 {r['vol']:8.3f} cm³　水密 {r['watertight']}")
    rep["parts_detail"] = rows

    # 壳体类零件（大而薄壁的）→ 壁厚
    print("\n【壁厚估计】（对体积最大的 3 个水密件）")
    rep["wall_thickness"] = []
    for p in sorted([p for p in parts if abs(p.volume) > 1000],
                    key=lambda p: -abs(p.volume))[:3]:
        wt = wall_thickness(p)
        s = p.bounds[1] - p.bounds[0]
        print(f"   {s[0]:7.2f} × {s[1]:7.2f} × {s[2]:7.2f} mm ⇒ 中位壁厚 "
              f"{'%.2f mm' % wt if wt else '测不到'}")
        rep["wall_thickness"].append({"size": [round(float(v), 2) for v in s], "median_mm": wt})

    # 孔位 + 与我们的板对照
    board = find_board(parts)
    print("\n【安装孔 / 板框 对照】")
    if board is not None:
        bs = board.bounds[1] - board.bounds[0]
        holes, axis = detect_holes(board, min_d=2.0, max_d=5.0)
        print(f"   检出板类件：{bs[0]:.2f} × {bs[1]:.2f} × {bs[2]:.2f} mm；"
              f"Φ2~5 孔 {len(holes)} 个")
        pts = sorted([(h["u"], h["v"], h["d"]) for h in holes])
        for u, v, d in pts:
            print(f"      ({u:8.2f}, {v:8.2f})  Φ{d:.2f}")
        spans = []
        for i in range(len(pts)):
            for j in range(i + 1, len(pts)):
                spans.append(round(math.dist(pts[i][:2], pts[j][:2]), 2))
        hit = [s for s in SPANS if any(abs(s - x) < 0.5 for x in spans)]
        print(f"   跨距命中我们的 3 孔（{SPANS}）：{hit if hit else '❌ 未命中'}")
        print(f"   板框与我们板（{BOARD_W}×{BOARD_H}）："
              f"{'✅ 一致' if abs(bs[0]-BOARD_W) < 1.0 and abs(bs[1]-BOARD_H) < 1.0 else '✗ 不一致'}")
        rep["board"] = {"size": [round(float(v), 2) for v in bs],
                        "holes": [[round(u, 2), round(v, 2), round(d, 2)] for u, v, d in pts],
                        "span_hits": hit,
                        "matches_ours": bool(abs(bs[0]-BOARD_W) < 1.0 and abs(bs[1]-BOARD_H) < 1.0)}
    else:
        print("   未找到板类件（可能不是 PCB 装配）")

    png = os.path.join(outdir, f"{stem}_四视图.png")
    try:
        render_views(m, png, size=2200, title=f"{stem}　{size[0]:.1f}×{size[1]:.1f}×{size[2]:.1f} mm")
    except Exception as e:
        print("渲染失败：", e)
    js = os.path.join(outdir, f"{stem}_摘要.json")
    json.dump(rep, open(js, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
    print(f"\n输出：{png}\n      {js}")


if __name__ == "__main__":
    main()

