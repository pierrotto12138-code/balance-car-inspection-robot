"""对比"改"板原理图与基线（只读）。

用法：
    python tools/sch_watch.py                  # 列出当前"改"板位号 + 与原始基线的差异
    python tools/sch_watch.py <P1.esch2>       # 指定文件
"""
import glob
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hardware"))
import importlib.util

_spec = importlib.util.spec_from_file_location(
    "sch_netlist", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hardware", "sch_netlist.py"))
nl = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(nl)

PROJ = os.environ.get("LCEDA_PROJECTS_REF") or os.path.join(
    os.environ.get("LCEDA_PROJECTS") or os.path.join(
        os.path.expanduser("~"), "Documents", "LCEDA-Pro", "projects"),
    "参考01_copy")
ORIG = os.path.join(PROJ, "sch", "Schematic1", "P1.esch2")   # "原"板（基线参照）
REQUIRED = ["J5", "J7", "J9", "J10", "J11", "J13", "J14", "J15", "J17", "J18",
            "R10", "R11", "R12", "R13", "R14",
            "C14", "C15", "C16", "C17", "C18", "C19"]
# 蜂鸣器采用"方案 A（PC5 直连模块 I/O）"，故 BZ1/R6/Q1/R7/R8 都不应存在
DROP = ["J6", "J8", "J12", "J16", "BZ1", "R6", "Q1", "R7", "R8"]


def des_of(rows):
    return sorted([r["des"] for r in rows if r["des"] and not r["des"].startswith("#PWR")], key=nl._sortkey)


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PROJ, "sch", "Schematic2", "P1.esch2")
    rows, nets = nl.parse(target)
    here = des_of(rows)
    o_rows, o_nets = nl.parse(ORIG)
    there = des_of(o_rows)

    print(f"# 目标：{target}")
    print(f"# 有位号元件 {len(here)} ｜ 网络名 {len(nets)}")
    print()
    print("现有位号：")
    for i in range(0, len(here), 12):
        print("  " + "  ".join(f"{x:<6}" for x in here[i:i + 12]))

    hr, tr = set(here), set(there)
    print()
    print(f"── 相对『原』板 ──")
    print(f"  仍有、原板也有：{' '.join(sorted(hr & tr, key=nl._sortkey)) or '（无）'}")
    print(f"  新增（原板没有）：{' '.join(sorted(hr - tr, key=nl._sortkey)) or '（无）'}")
    print(f"  原板有、这里没有：{' '.join(sorted(tr - hr, key=nl._sortkey)) or '（无）'}")

    print()
    print("── 对照本计划 ──")
    miss = [d for d in REQUIRED if d not in hr]
    print(f"  计划要求但【还没有】的（{len(miss)}）：{' '.join(miss) or '（全齐 OK）'}")
    leftover = [d for d in DROP if d in hr]
    print(f"  计划要删但【仍存在】的（{len(leftover)}）：{' '.join(leftover) or '（已清 OK）'}")

    # 网络名
    print()
    key_nets = ["MQ2_AO", "MQ135_AO", "PB12", "PC3"]
    for n in key_nets:
        print(f"  网络 {n:<10} {'存在' if n in nets else '不存在'}")

    # 立创/文件层面的 footprint 值
    print()
    print("── 新增件的封装属性检查（改名后应为 0805 / 电解）──")
    for r in sorted(rows, key=lambda r: nl._sortkey(r["des"] or "zz")):
        if r["des"] in ("R10", "R11", "R12", "R13", "R14", "C14", "C15", "C16", "C17", "C18", "C19"):
            print(f"  {r['des']:<5} value={r['value']!r:<12} footprint={r['footprint']!r}")


if __name__ == "__main__":
    main()
