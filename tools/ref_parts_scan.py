# -*- coding: utf-8 -*-
r"""批量体检一堆 3D 模型（STEP/IGES/STL…）—— 先缓存 STL，再在 STL 上算，避免反复读 STEP。

用法：
  python tools/ref_parts_scan.py <目录或文件…> [-o 输出目录] [--slices 8] [--wall-samples 200]

产出（默认落在模型目录下 `intake_cache\`）：
  intake_cache\<件名>\part_XX.stl      逐实体 STL（便于后续装配/爆炸图/明细栏）
  intake_cache\<件名>\manifest.json    逐实体：尺寸/体积/水密/孔位/壁厚
  A2_取数_逐件体检.json                 汇总表（每件一行）
"""
import argparse, json, math, os, sys, time
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mesh_tools import load_model, merged, detect_holes
from step_intake import wall_thickness

EXTS = (".step", ".stp", ".igs", ".iges", ".stl", ".obj", ".ply", ".glb")


def scan_one(path, outdir, slices, wall_samples):
    import trimesh
    t0 = time.time()
    stem = os.path.splitext(os.path.basename(path))[0][:40]
    sub = os.path.join(outdir, stem)
    os.makedirs(sub, exist_ok=True)
    parts, names = load_model(path)
    m = merged(parts)
    size = m.bounds[1] - m.bounds[0]

    per = []
    for i, p in enumerate(parts):
        s = p.bounds[1] - p.bounds[0]
        try:
            p.export(os.path.join(sub, f"part_{i:02d}.stl"))
        except Exception:
            pass
        per.append({"i": i, "size": [round(float(v), 2) for v in s],
                    "vol": round(float(p.volume) / 1000, 3),
                    "watertight": bool(p.is_watertight)})

    holes, axis = detect_holes(m, min_d=2.0, max_d=5.0, n_slices=slices)
    pts = sorted([(h["u"], h["v"], h["d"]) for h in holes])
    spans = sorted({round(math.dist(a[:2], b[:2]), 2)
                    for i, a in enumerate(pts) for b in pts[i + 1:]})

    wts = []
    for p in sorted([p for p in parts if abs(p.volume) > 500], key=lambda q: -abs(q.volume))[:3]:
        w = wall_thickness(p, n=wall_samples)
        wts.append({"size": [round(float(v), 2) for v in p.bounds[1] - p.bounds[0]],
                    "median_mm": round(w, 2) if w else None})

    rec = {"file": os.path.basename(path), "solids": len(parts),
           "size": [round(float(v), 2) for v in size],
           "vol": round(float(m.volume) / 1000, 2), "watertight": bool(m.is_watertight),
           "holes": [[round(u, 2), round(v, 2), round(d, 2)] for u, v, d in pts],
           "spans": spans, "walls": wts, "per_solid": per,
           "secs": round(time.time() - t0, 1)}
    json.dump(rec, open(os.path.join(sub, "manifest.json"), "w", encoding="utf-8"),
              ensure_ascii=False, indent=2)
    print(f"  {rec['file'][:40]:<40} 实体{rec['solids']:>3}  {rec['size'][0]:7.1f}×{rec['size'][1]:6.1f}×"
          f"{rec['size'][2]:6.1f}  体积{rec['vol']:8.2f}  Φ2~5孔{len(pts):>3}  "
          f"壁厚{(wts[0]['median_mm'] if wts and wts[0]['median_mm'] else 0):5.2f}  {rec['secs']}s",
          flush=True)
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("-o", "--out", default=None)
    ap.add_argument("--slices", type=int, default=8)
    ap.add_argument("--wall-samples", type=int, default=200)
    a = ap.parse_args()

    files = []
    for p in a.paths:
        if os.path.isdir(p):
            files += [os.path.join(p, f) for f in sorted(os.listdir(p))
                      if f.lower().endswith(EXTS) and not f.startswith("~$")]
        else:
            files.append(p)
    outdir = a.out or os.path.join(os.path.dirname(os.path.abspath(files[0])), "intake_cache")
    os.makedirs(outdir, exist_ok=True)
    print(f"共 {len(files)} 个模型 → 缓存到 {outdir}\n")
    rows = []
    for f in files:
        try:
            rows.append(scan_one(f, outdir, a.slices, a.wall_samples))
        except Exception as e:
            print(f"  {os.path.basename(f)[:40]:<40} 失败：{type(e).__name__}: {str(e)[:60]}", flush=True)
            rows.append({"file": os.path.basename(f), "err": f"{type(e).__name__}: {str(e)[:80]}"})
    js = os.path.join(outdir, "A2_取数_逐件体检.json")
    json.dump(rows, open(js, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
    print(f"\n汇总已存：{js}")


if __name__ == "__main__":
    main()
