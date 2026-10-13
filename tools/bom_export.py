#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从立创EDA专业版的 **完工快照**离线导出 BOM（位号 / 器件 / 型号 / 封装 / 立创料号 / 数量）。

为什么离线导出：任务书要求 A1 提交"BOM 表"，而打开 EDA 导出需要人在 GUI 里点
`API Gateway → Reconnect`。快照里的 `.epcb2` 本身就带齐了 BOM 需要的字段，
所以这个脚本**只读文件**就能生成，谁都能复跑、可核验。

数据来源（`hardware/硬件工程_完工版_*/pcb/PCB2.epcb2`，格式：每行 `{头}||{负载}`）：
  · `ATTR` 且 `key=Designator` 且值不含 `?`  → 真实位号；其 `parentId` 指向 **COMPONENT 记录**
  · `COMPONENT` → `attrs.DeviceName`（JSON，含器件名）等
  · `META` → 该器件的**库信息**：`Supplier Part`（立创料号 C 开头）、`Manufacturer`、
    `Manufacturer Part`、`Supplier Footprint`、`LCSC Part Name`（描述）
  位号与库信息的关联：`COMPONENT.attrs.DeviceName.name` ↔ `META.title`（器件名相同即同一器件）

用法：
    python tools/bom_export.py                 # 打印统计 + 前若干行
    python tools/bom_export.py --md  <文件>     # 写 Markdown 表格
    python tools/bom_export.py --csv <文件>     # 写 CSV（Excel 可直接打开）
    python tools/bom_export.py --pcb <epcb2>   # 指定 PCB 文件（默认取最新快照）
"""
import argparse
import collections
import csv
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "hardware"))


def default_pcb():
    """默认取 <仓库根>/hardware 下最新快照里的 PCB2.epcb2。"""
    hw = os.path.join(ROOT, "hardware")
    best = None
    for name in sorted(os.listdir(hw)) if os.path.isdir(hw) else []:
        p = os.path.join(hw, name, "pcb", "PCB2.epcb2")
        if os.path.exists(p):
            if best is None or os.path.getmtime(p) > os.path.getmtime(best):
                best = p
    if best is None:
        raise SystemExit("找不到完工快照里的 PCB2.epcb2（可用 --pcb 指定）")
    return best


def load(path):
    import sch_netlist as sn          # 复用已有的 EDA 文件解析器（只读）
    return sn.load(path)


def natural_des_key(des):
    """R2 < R10（位号自然序）"""
    import re
    m = re.match(r"^([A-Za-z]+)(\d+)", des or "")
    return (m.group(1), int(m.group(2))) if m else (des or "", 0)


def build(path):
    recs = load(path)

    # 1) 位号 → 所属 COMPONENT 记录 id
    des_parent = {}
    for r in recs:
        if r["type"] == "ATTR" and r["p"].get("key") == "Designator":
            v = r["p"].get("value") or ""
            if v and "?" not in v:
                des_parent[v] = r["p"].get("parentId")

    # 2) COMPONENT 记录 → 器件名等属性
    comp_by_id = {}
    for r in recs:
        if r["type"] == "COMPONENT":
            comp_by_id[r["id"]] = r["p"].get("attrs", {})

    # 3) 器件库信息（META）：title(器件名) → 料号等
    lib_by_title = {}
    for r in recs:
        if r["type"] == "META":
            p = r["p"]
            a = p.get("attributes") or {}
            title = p.get("title") or ""
            if title and a:
                lib_by_title.setdefault(title, a)

    rows = []
    for des, pid in sorted(des_parent.items(), key=lambda kv: natural_des_key(kv[0])):
        attrs = comp_by_id.get(pid, {})
        dev_name, dev_uuid = "", ""
        dn = attrs.get("DeviceName") or ""
        if dn:
            try:
                j = json.loads(dn)
                dev_name = j.get("name", "")
                dev_uuid = j.get("uuid", "")
            except (ValueError, TypeError):
                dev_name = dn
        lib = lib_by_title.get(dev_name, {})
        rows.append({
            "des": des,
            "device": dev_name,
            "value": attrs.get("Value", "") or "",
            "footprint": lib.get("Supplier Footprint", ""),
            "lcsc": lib.get("Supplier Part", ""),
            "mfr": lib.get("Manufacturer", ""),
            "mfr_part": lib.get("Manufacturer Part", "") or dev_name,
            "desc": lib.get("LCSC Part Name", ""),
            "bom": lib.get("Add into BOM", ""),
        })
    return rows, lib_by_title


def group(rows):
    """按 (器件, 料号, 封装) 合并，收集位号列表 —— 这才是 BOM 的常规形态。"""
    buckets = collections.OrderedDict()
    for r in rows:
        key = (r["device"], r["lcsc"], r["footprint"])
        b = buckets.setdefault(key, {"device": r["device"], "lcsc": r["lcsc"],
                                     "footprint": r["footprint"], "mfr": r["mfr"],
                                     "mfr_part": r["mfr_part"], "desc": r["desc"],
                                     "value": r["value"], "des": []})
        b["des"].append(r["des"])
        if not b["value"] and r["value"]:
            b["value"] = r["value"]
    return list(buckets.values())


def main():
    ap = argparse.ArgumentParser(description="从完工快照离线导出 BOM")
    ap.add_argument("--pcb", default=None, help="PCB2.epcb2 路径（默认取最新快照）")
    ap.add_argument("--md", default=None, help="写 Markdown 表格到该文件")
    ap.add_argument("--csv", default=None, help="写 CSV 到该文件")
    args = ap.parse_args()

    pcb = args.pcb or default_pcb()
    rows, lib = build(pcb)
    groups = group(rows)

    have_lcsc = [r for r in rows if r["lcsc"]]
    print(f"PCB: {os.path.relpath(pcb, ROOT)}")
    print(f"位号: {len(rows)} 个（唯一 {len(set(r['des'] for r in rows))}）")
    print(f"器件库条目(META): {len(lib)} 条")
    print(f"已匹配到立创料号: {len(have_lcsc)}/{len(rows)}")
    missing = [r["des"] for r in rows if not r["lcsc"]]
    if missing:
        print("  未匹配到料号的位号:", ", ".join(sorted(missing, key=natural_des_key)))
    kinds = collections.Counter(r["des"][0] for r in rows if r["des"])
    print("按位号首字母统计:", dict(sorted(kinds.items())))
    print(f"合并后 BOM 行数: {len(groups)}")
    print("\n前 12 行：")
    print(f"  {'位号':<22}{'器件':<28}{'封装':<10}{'料号':<12}")
    for g in groups[:12]:
        print(f"  {','.join(g['des'])[:20]:<22}{g['device'][:26]:<28}"
              f"{g['footprint'][:8]:<10}{g['lcsc']:<12}")

    if args.csv:
        os.makedirs(os.path.dirname(os.path.abspath(args.csv)), exist_ok=True)
        with open(args.csv, "w", newline="", encoding="utf-8-sig") as fh:
            w = csv.writer(fh)
            w.writerow(["序号", "数量", "位号", "器件名", "型号(厂商料号)", "封装",
                        "立创料号", "厂商", "描述"])
            for i, g in enumerate(groups, 1):
                w.writerow([i, len(g["des"]), " ".join(g["des"]), g["device"],
                            g["mfr_part"], g["footprint"], g["lcsc"], g["mfr"], g["desc"]])
        print("\n已写 CSV:", os.path.relpath(args.csv, ROOT))

    if args.md:
        os.makedirs(os.path.dirname(os.path.abspath(args.md)), exist_ok=True)
        with open(args.md, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("| # | 数量 | 位号 | 器件 | 型号 | 封装 | 立创料号 | 厂商 |\n")
            fh.write("|---|---|---|---|---|---|---|---|\n")
            for i, g in enumerate(groups, 1):
                fh.write(f"| {i} | {len(g['des'])} | {' '.join(g['des'])} | {g['device']} | "
                         f"{g['mfr_part']} | {g['footprint']} | {g['lcsc']} | {g['mfr']} |\n")
        print("已写 Markdown:", os.path.relpath(args.md, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
