# -*- coding: utf-8 -*-
r"""把 PCB 3D STEP 压成"SW 秒开、可编辑、可保存"的**接口包络**模型。

为什么这么做（2026-10-17 实测）：
  SolidWorks 2024 SP2 起强制走 3D Interconnect，直接导 30 MB 的 PCB 3D（53 件 / 11658 个
  样式实体）经常出现：打不开 → 打开后视图空白 → 打开了也存不了（链接文档）。把整板换成
  "板 + 每个器件的包围盒"后，文件只剩几百 KB、纯实体、无链接，SW 打开即用，且包围盒
  尺寸就是装配/避让真正需要的量。

输入：任意 PCB 3D STEP（默认本仓库 hardware\eda_export_20261016\3D_PCB2_2026-10-16.step）
输出：<out>.step（单件复合体：1 块板 + 每器件 1 个盒子）、<out>_清单.csv、<out>_参数.json

用法：
  python tools\pcb3d_envelope.py [-i 输入.step] [-o 输出前缀] [--min-h 1.0]
"""
import argparse
import csv
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from OCP.BRep import BRep_Builder
from OCP.BRepBuilderAPI import (BRepBuilderAPI_MakeEdge, BRepBuilderAPI_MakeFace,
                                BRepBuilderAPI_MakeWire, BRepBuilderAPI_Transform)
from OCP.BRepGProp import BRepGProp
from OCP.BRepPrimAPI import BRepPrimAPI_MakeBox, BRepPrimAPI_MakePrism
from OCP.GC import GC_MakeArcOfCircle
from OCP.GProp import GProp_GProps
from OCP.STEPControl import STEPControl_AsIs, STEPControl_Writer
from OCP.TopoDS import TopoDS_Compound
from OCP.gp import gp_Pnt, gp_Trsf, gp_Vec

from mesh_tools import find_board, load_model

DEFAULT_IN = r"D:\260921Task\hardware\eda_export_20261016\3D_PCB2_2026-10-16.step"
BOARD = (100.0, 65.0)
R_CORNER = 5.0
BOARD_T = 1.6


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
    t = gp_Trsf()
    t.SetTranslation(gp_Vec(0, 0, z0))
    return BRepBuilderAPI_Transform(prism, t, True).Shape()


def box(x0, y0, z0, x1, y1, z1):
    return BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, z0), gp_Pnt(x1, y1, z1)).Shape()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", "--input", default=DEFAULT_IN)
    ap.add_argument("-o", "--out", default=os.path.splitext(DEFAULT_IN)[0] + "_接口包络")
    ap.add_argument("--min-h", type=float, default=1.0, help="低于该高度的器件（贴片）不建盒子")
    a = ap.parse_args()

    parts, names = load_model(a.input)
    board = find_board(parts)
    bx0, by0, bz0 = board.bounds[0]
    bx1, by1, bz1 = board.bounds[1]
    print(f"板：{bx1-bx0:.2f} × {by1-by0:.2f} × {bz1-bz0:.2f} mm（原点在板左下 {bx0:.3f}, {by0:.3f}, {bz0:.3f}）")

    # 单位：mesh_tools 读 STEP 后按 mm 输出
    t = gp_Trsf()
    t.SetTranslation(gp_Vec(-bx0, -by0, -bz0))
    plate = BRepBuilderAPI_Transform(
        rounded_rect_prism(BOARD[0], BOARD[1], R_CORNER, 0.0, BOARD_T), t, True).Shape()

    comp = TopoDS_Compound()
    BRep_Builder().MakeCompound(comp)
    BRep_Builder().Add(comp, plate)

    rows = []
    n_box = 0
    for p, nm in zip(parts, names):
        if p is board:
            continue
        (x0, y0, z0), (x1, y1, z1) = p.bounds
        if z1 - z0 < a.min_h:
            continue
        b = box(x0 - bx0, y0 - by0, z0 - bz0, x1 - bx0, y1 - by0, z1 - bz0)
        BRep_Builder().Add(comp, b)
        n_box += 1
        rows.append({
            "器件": nm,
            "中心X": round((x0 + x1) / 2 - bx0, 2),
            "中心Y": round((y0 + y1) / 2 - by0, 2),
            "宽X": round(x1 - x0, 2),
            "宽Y": round(y1 - y0, 2),
            "高Z": round(z1 - z0, 2),
            "底Z": round(z0 - bz0, 2),
            "顶Z": round(z1 - bz0, 2),
        })

    out = a.out + ".step"
    w = STEPControl_Writer()
    w.Transfer(comp, STEPControl_AsIs)
    st = w.Write(out)
    size = os.path.getsize(out) / 1024
    print(f"包络件：板 1 + 器件盒 {n_box}（跳过高度 < {a.min_h} mm 的贴片）")
    print(f"写出：{out}  {size:.0f} KB  ({st})")

    with open(a.out + "_清单.csv", "w", encoding="utf-8-sig", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        wr.writeheader()
        wr.writerows(rows)
    tops = sorted(rows, key=lambda r: -r["顶Z"])[:8]
    with open(a.out + "_参数.json", "w", encoding="utf-8") as f:
        json.dump({"板": {"宽X": BOARD[0], "宽Y": BOARD[1], "厚": BOARD_T, "圆角": R_CORNER},
                   "器件盒数": n_box, "最高 8 件": tops}, f, ensure_ascii=False, indent=2)
    print("最高 8 件（板面向上 mm）：")
    for r in tops:
        print(f"   {r['器件']:<34} 顶 {r['顶Z']:>6.2f}   中心 ({r['中心X']:.1f}, {r['中心Y']:.1f})")


if __name__ == "__main__":
    main()
