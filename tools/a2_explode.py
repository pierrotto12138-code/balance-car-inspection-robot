# -*- coding: utf-8 -*-
r"""出「爆炸图 + 明细栏」——给 A2 的装配类图纸用（纯离线，matplotlib + PIL）。

用法：
  python tools/a2_explode.py --parts "下壳=path.stl" "上壳=path.stl" "PCB=path.stl" \
      -o 爆炸图.png [--axis z] [--gap 25] [--bom bom.csv]

· 每个件沿 `--axis` 依次偏移 `--gap` mm（从下往上堆叠展示），件间用虚线引出序号
· `--bom` 给一份 CSV（列：序号,名称,数量,材料,备注），会画在图纸右下角的明细栏里
"""
import argparse, csv, math, os, sys
import numpy as np
import trimesh

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

COLORS = ["#8fb8de", "#c8d9a8", "#e0b98f", "#b9a8d9", "#9fd4c9", "#e3a3a3", "#c9c9c9"]


def load(path):
    m = trimesh.load(path, force="mesh")
    if isinstance(m, trimesh.Scene):
        m = trimesh.util.concatenate([g for g in m.geometry.values()])
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--parts", nargs="+", required=True, help="名称=路径 …")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--axis", default="z", choices=["x", "y", "z"])
    ap.add_argument("--gap", type=float, default=25.0)
    ap.add_argument("--bom", default=None)
    ap.add_argument("--title", default="A2 外壳装配 爆炸图（示意）")
    a = ap.parse_args()

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection
    for f in ("SimHei", "Microsoft YaHei", "SimSun"):
        try:
            matplotlib.rcParams["font.sans-serif"] = [f]; break
        except Exception:
            pass
    matplotlib.rcParams["axes.unicode_minus"] = False

    items = []
    for spec in a.parts:
        name, _, path = spec.partition("=")
        m = load(path)
        c = (m.bounds[0] + m.bounds[1]) / 2
        m = m.copy()
        m.apply_translation(-c)                     # 移到原点，便于按序堆叠
        items.append([name, m])

    ax_i = {"x": 0, "y": 2, "z": 2}[a.axis]         # 画布竖直方向取 z 或 y
    fig = plt.figure(figsize=(11.7, 8.3), dpi=200)  # A4 横
    if a.bom:
        ax = fig.add_axes([0.02, 0.36, 0.62, 0.60], projection="3d")
    else:
        ax = fig.add_axes([0.02, 0.04, 0.96, 0.90], projection="3d")
    ax.set_axis_off()
    shift = 0.0
    light = np.array([0.4, 0.5, 0.75]); light /= np.linalg.norm(light)
    for i, (name, m) in enumerate(items):
        mm = m.copy()
        off = np.zeros(3); off[{"x": 0, "y": 1, "z": 2}[a.axis]] = shift
        mm.apply_translation(off)
        base = COLORS[i % len(COLORS)]
        rgb = np.array(matplotlib.colors.to_rgb(base))
        shade = np.clip(mm.face_normals @ light, 0, 1) * 0.5 + 0.5
        cols = np.c_[np.clip(rgb[0] * shade, 0, 1), np.clip(rgb[1] * shade, 0, 1),
                     np.clip(rgb[2] * shade, 0, 1), np.ones_like(shade)]
        ax.add_collection3d(Poly3DCollection(mm.vertices[mm.faces], facecolors=cols,
                                             edgecolors=(0.2, 0.2, 0.2, 0.25), linewidths=0.1))
        c = (mm.bounds[0] + mm.bounds[1]) / 2
        ax.text(c[0], c[1], c[2] + 6, f"{i+1}", fontsize=13, fontweight="bold", color="#a33")
        shift += a.gap
    allb = np.vstack([m.bounds for _, m in items])
    ax.set_xlim(allb[:, 0].min() - 10, allb[:, 0].max() + 10)
    ax.set_ylim(allb[:, 1].min() - 10, allb[:, 1].max() + 10)
    ax.set_zlim(allb[:, 2].min() - 10, allb[:, 2].max() + a.gap * len(items))
    ax.view_init(elev=22, azim=-60)
    ax.set_title(a.title, fontsize=15, pad=2)

    if a.bom and os.path.exists(a.bom):
        rows = list(csv.reader(open(a.bom, encoding="utf-8-sig")))
        ax2 = fig.add_axes([0.66, 0.06, 0.32, 0.86]); ax2.set_axis_off()
        tb = ax2.table(cellText=rows[1:], colLabels=rows[0], loc="upper center", cellLoc="left")
        tb.auto_set_font_size(False); tb.set_fontsize(8); tb.scale(1, 1.5)
        for (r, c), cell in tb.get_celld().items():
            if r == 0:
                cell.set_facecolor("#d9e2f3"); cell.set_text_props(fontweight="bold")

    fig.savefig(a.out, facecolor="white")
    if a.out.lower().endswith(".png"):
        fig.savefig(os.path.splitext(a.out)[0] + ".pdf", facecolor="white")
    print("已出图:", a.out, "+ 同名 PDF")


if __name__ == "__main__":
    main()
