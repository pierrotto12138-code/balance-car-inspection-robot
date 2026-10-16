# -*- coding: utf-8 -*-
"""PCB2.epcb2 离线解析（修正版）：板框/圆角/孔 + 器件坐标 + 座外形（用 footprintPrimitives）。"""
import json, os, csv, math

import sys
SNAP = sys.argv[1] if len(sys.argv) > 1 else r"D:\260921Task\hardware\硬件工程_完工版_20261008_最终\pcb\PCB2.epcb2"
OUT = r"D:\260921Task_私有资料\正式作业提交\_A2审阅"
MIL = 0.0254
dec = json.JSONDecoder()
recs = []
for line in open(SNAP, encoding="utf-8"):
    i = line.find("||")
    if i < 0: continue
    try: head = dec.raw_decode(line[:i])[0]
    except json.JSONDecodeError: continue
    rest = line[i+2:].rstrip()
    if rest.endswith("|"): rest = rest[:-1]
    try: p = dec.raw_decode(rest)[0]
    except json.JSONDecodeError: continue
    recs.append({"type": head.get("type"), "id": head.get("id"), "p": p})

# ---------- 板框：正确走 path（L / ARC 后跟坐标；ARC 多一个角度）
def walk(path):
    pts, i = [], 0
    if len(path) >= 2 and isinstance(path[0], (int, float)) and isinstance(path[1], (int, float)):
        pts.append((float(path[0]), float(path[1]))); i = 2
    while i < len(path):
        cmd = str(path[i]).upper(); i += 1
        if cmd == "L":
            if i + 1 < len(path): pts.append((float(path[i]), float(path[i+1]))); i += 2
        elif cmd == "ARC":
            if i + 2 < len(path): pts.append((float(path[i+1]), float(path[i+2]))); i += 3
        else:
            i += 1
    return pts

outline_pts, holes = [], []
for r in recs:
    if r["type"] == "POLY" and r["p"].get("polyType") == "BOARD_OUTLINE":
        path = r["p"].get("path") or []
        if path and str(path[0]).upper() == "CIRCLE":
            holes.append((float(path[1]), float(path[2]), float(path[3])))
        else:
            outline_pts += walk(path)

xs = [p[0] for p in outline_pts]; ys = [p[1] for p in outline_pts]
OX, OY = min(xs), min(ys)
W, H = (max(xs)-OX)*MIL, (max(ys)-OY)*MIL
print(f"板框 {W:.2f} × {H:.2f} mm｜x∈[{min(xs):.2f},{max(xs):.2f}] y∈[{min(ys):.2f},{max(ys):.2f}] mil")
# 圆角：找 L 段两侧的 90° 折角，半径 = 相邻坐标差
corners = []
for i in range(len(outline_pts)):
    a, b = outline_pts[i], outline_pts[(i+1) % len(outline_pts)]
    dx, dy = abs(b[0]-a[0]), abs(b[1]-a[1])
    if dx > 1 and dy > 1 and abs(dx-dy) < 1: corners.append(round(dx*MIL, 2))
print("圆角半径（mm）：", sorted(set(corners)), "  ← 应为 R5.0")
print("安装孔：")
for cx, cy, r in sorted(holes, key=lambda c: (-c[1], c[0])):
    print(f"  ({(cx-OX)*MIL:6.2f}, {(cy-OY)*MIL:6.2f}) mm  ⌀{2*r*MIL:.2f} mm")

def rel(x, y): return round((float(x)-OX)*MIL, 2), round((float(y)-OY)*MIL, 2)

# ---------- 位号 + 器件
desig = {}
for r in recs:
    if r["type"] == "ATTR" and r["p"].get("key") == "Designator":
        desig.setdefault(str(r["p"].get("parentId")), r["p"].get("value"))

def fp_bbox(prims, angle):
    """从 footprintPrimitives 取焊盘/图元相对坐标，旋转后返回相对器件原点的包围盒（mil）"""
    pts = []
    def rec(o):
        if isinstance(o, dict):
            for k in ("centerX", "centerY"):
                pass
            if "centerX" in o and "centerY" in o:
                pts.append((float(o["centerX"] or 0), float(o["centerY"] or 0)))
            for v in o.values(): rec(v)
        elif isinstance(o, list):
            nums = [v for v in o if isinstance(v, (int, float))]
            for k in range(0, len(nums)-1, 2):
                pts.append((float(nums[k]), float(nums[k+1])))
    rec(prims)
    if not pts: return None
    a = math.radians(float(angle or 0))
    rot = [(x*math.cos(a)-y*math.sin(a), x*math.sin(a)+y*math.cos(a)) for x, y in pts]
    return (min(p[0] for p in rot), min(p[1] for p in rot), max(p[0] for p in rot), max(p[1] for p in rot))

# 每个器件的引脚数（PAD_NET）
pincount = {}
for r in recs:
    if r["type"] != "PAD_NET": continue
    try: _, cid, num, _p = json.loads(r["id"])
    except (ValueError, TypeError): continue
    pincount[str(cid)] = pincount.get(str(cid), 0) + 1

rows = []
for r in recs:
    if r["type"] != "COMPONENT": continue
    p = r["p"]; cid = str(r["id"])
    d = desig.get(cid) or ""
    if not d: continue
    dev = ""
    try: dev = json.loads((p.get("attrs") or {}).get("DeviceName", "{}")).get("name", "")
    except Exception: pass
    X, Y = rel(p.get("x"), p.get("y"))
    bb = fp_bbox(p.get("footprintPrimitives"), p.get("angle"))
    box = ""
    if bb:
        x0, y0 = rel(p["x"]+bb[0], p["y"]+bb[1]); x1, y1 = rel(p["x"]+bb[2], p["y"]+bb[3])
        box = f"{x0:.1f}~{x1:.1f} × {y0:.1f}~{y1:.1f}"
    rows.append({"des": d, "x": X, "y": Y, "rot": round(float(p.get("angle") or 0), 1),
                 "pins": pincount.get(cid, 0), "dev": dev, "box": box})
rows.sort(key=lambda r: (r["des"][:1], r["des"]))

os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, "A2_接口坐标表_20261016.csv"), "w", newline="", encoding="utf-8-sig") as f:
    w = csv.writer(f); w.writerow(["位号", "中心X_mm", "中心Y_mm", "旋转_deg", "引脚数", "外形范围_mm(板左下角)", "器件名"])
    for r in rows: w.writerow([r["des"], r["x"], r["y"], r["rot"], r["pins"], r["box"], r["dev"]])

print(f"\n器件 {len(rows)} 个，其中座/接插件：")
for r in rows:
    if r["des"][:1] in ("J", "C") and r["des"][:2] in ("J1", "J2", "J5", "J7", "J9", "J10", "J11", "J13", "J14", "J15", "J17", "J18", "CN"):
        print(f"  {r['des']:<5} ({r['x']:>6.2f},{r['y']:>6.2f}) rot={r['rot']:>6} {r['pins']:>2}脚  外形 {r['box']}")

json.dump({"board_mm": [round(W, 2), round(H, 2)], "origin_mil": [OX, OY],
           "corners_mm": sorted(set(corners)),
           "holes_mm": [[round((c[0]-OX)*MIL, 2), round((c[1]-OY)*MIL, 2), round(2*c[2]*MIL, 2)] for c in holes],
           "comps": rows}, open(os.path.join(OUT, "A2_接口摘要.json"), "w", encoding="utf-8"), ensure_ascii=False, indent=1)
print("\n已写 A2_接口坐标表_20261016.csv + A2_接口摘要.json")

