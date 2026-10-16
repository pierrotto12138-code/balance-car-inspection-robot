# -*- coding: utf-8 -*-
"""读 IGES/STEP 实体清单 + 可选曲面细分导出 STL（OpenCASCADE/OCP）。

用法：
  python tools/iges_read.py list  <文件.igs|.iges|.stp>            # 只列实体/尺寸（快）
  python tools/iges_read.py dump  <文件> -o out.stl [--tol 0.2]    # 细分并导出 STL（慢）
"""
import argparse, os, sys, numpy as np

from OCP.IGESControl import IGESControl_Reader
from OCP.STEPControl import STEPControl_Reader
from OCP.IFSelect import IFSelect_RetDone
from OCP.TopExp import TopExp_Explorer
from OCP.TopAbs import TopAbs_SOLID, TopAbs_SHELL, TopAbs_FACE, TopAbs_COMPOUND, TopAbs_REVERSED
from OCP.TopoDS import TopoDS
from OCP.BRepBndLib import BRepBndLib
from OCP.Bnd import Bnd_Box
from OCP.GProp import GProp_GProps
from OCP.BRepGProp import BRepGProp
from OCP.BRepMesh import BRepMesh_IncrementalMesh
from OCP.BRep import BRep_Tool
from OCP.TopLoc import TopLoc_Location
from OCP.TopoDS import TopoDS_Face


def read_shape(path):
    ext = os.path.splitext(path)[1].lower()
    if ext in (".igs", ".iges"):
        r = IGESControl_Reader()
    else:
        r = STEPControl_Reader()
    status = r.ReadFile(path)
    if status != IFSelect_RetDone:
        raise SystemExit(f"读取失败：{path}（status={status}）")
    r.TransferRoots()
    return r.OneShape(), r


def bbox_of(shape):
    b = Bnd_Box(); BRepBndLib.Add_s(shape, b)
    if b.IsVoid():
        return None
    mn, mx = b.CornerMin(), b.CornerMax()
    return (mn.X(), mn.Y(), mn.Z(), mx.X(), mx.Y(), mx.Z())


def volume_of(shape):
    props = GProp_GProps(); BRepGProp.VolumeProperties_s(shape, props)
    return props.Mass()


def count(shape, kind):
    n = 0; ex = TopExp_Explorer(shape, kind)
    while ex.More():
        n += 1; ex.Next()
    return n


def cmd_list(a):
    shape, _ = read_shape(a.file)
    print(f"文件：{os.path.basename(a.file)}  ({os.path.getsize(a.file)/1048576:.1f} MB)")
    print(f"总形状：SOLID {count(shape, TopAbs_SOLID)} · SHELL {count(shape, TopAbs_SHELL)} · "
          f"FACE {count(shape, TopAbs_FACE)}")
    bb = bbox_of(shape)
    if bb:
        print(f"整体包围盒（原始单位，IGES 多为 mm）：X {bb[0]:.1f}~{bb[3]:.1f}　Y {bb[1]:.1f}~{bb[4]:.1f}　Z {bb[2]:.1f}~{bb[5]:.1f}")
        print(f"整体尺寸：{bb[3]-bb[0]:.2f} × {bb[4]-bb[1]:.2f} × {bb[5]-bb[2]:.2f}")
        print(f"整体体积：{volume_of(shape)/1000:.2f} cm³")

    print("\n实体清单（前 60）：")
    ex = TopExp_Explorer(shape, TopAbs_SOLID)
    i = 0
    while ex.More() and i < 60:
        s = TopoDS.Solid(ex.Current())
        b = bbox_of(s)
        v = volume_of(s)
        if b:
            print(f"  #{i:<3} {b[3]-b[0]:7.2f} × {b[4]-b[1]:7.2f} × {b[5]-b[2]:7.2f} mm   "
                  f"X {b[0]:8.2f}~{b[3]:8.2f}  Y {b[1]:8.2f}~{b[4]:8.2f}  Z {b[2]:8.2f}~{b[5]:8.2f}   "
                  f"体积 {v/1000:8.2f} cm³")
        i += 1
        ex.Next()
    print(f"  …共 {count(shape, TopAbs_SOLID)} 个实体")


def tessellate(shape, tol=0.2):
    BRepMesh_IncrementalMesh(shape, tol, False, 0.5, True)
    verts, faces = [], []
    ex = TopExp_Explorer(shape, TopAbs_FACE)
    while ex.More():
        face = TopoDS.Face(ex.Current())
        loc = TopLoc_Location()
        tri = BRep_Tool.Triangulation_s(face, loc)
        if tri is not None:
            trsf = loc.Transformation()
            base = len(verts)
            rev = (face.Orientation() == TopAbs_REVERSED)   # ★ 面朝向决定三角面绕向
            for k in range(1, tri.NbNodes() + 1):
                p = tri.Node(k).Transformed(trsf)
                verts.append((p.X(), p.Y(), p.Z()))
            for k in range(1, tri.NbTriangles() + 1):
                t = tri.Triangle(k)
                a1, a2, a3 = t.Get()
                if rev:
                    a2, a3 = a3, a2
                faces.append((base + a1 - 1, base + a2 - 1, base + a3 - 1))
        ex.Next()
    return np.array(verts, dtype=float), np.array(faces, dtype=np.int64)


def cmd_dump(a):
    shape, _ = read_shape(a.file)
    v, f = tessellate(shape, a.tol)
    print(f"细分完成：{len(v):,} 顶点 / {len(f):,} 三角面")
    import trimesh
    m = trimesh.Trimesh(vertices=v, faces=f, process=False)
    m.export(a.out)
    print("已导出:", a.out, " 尺寸:", np.round(m.bounds[1]-m.bounds[0], 2))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="读 IGES/STEP：实体清单 / 细分导出")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list"); p.add_argument("file"); p.set_defaults(func=cmd_list)
    p = sub.add_parser("dump"); p.add_argument("file"); p.add_argument("-o", "--out", required=True)
    p.add_argument("--tol", type=float, default=0.2); p.set_defaults(func=cmd_dump)
    args = ap.parse_args(); args.func(args)


