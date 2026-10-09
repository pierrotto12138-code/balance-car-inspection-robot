"""读取嘉立创 EDA Pro `.eprj3` 工程里的原理图（.esch2），导出只读清单。

用途：在 **EDA 关闭时** 对"改"板原理图做事实核查与改动前后的比对，
不写任何文件到工程目录，安全。

用法：
    # 列出某张原理图的 位号 / 器件 / 值 / 网络名
    python hardware/sch_netlist.py <P1.esch2>

    # 两张图对照（推荐：改板 vs 原板），列出各自多出来的位号
    python hardware/sch_netlist.py <改板.esch2> --diff <原板.esch2>

    # 导出 CSV（便于 Excel 核对）
    python hardware/sch_netlist.py <P1.esch2> --csv out.csv

工程内路径示例：
    ...\\LCEDA-Pro\\projects\\参考01_copy\\sch\\Schematic2\\P1.esch2   ← "改"板
    ...\\LCEDA-Pro\\projects\\参考01_copy\\sch\\Schematic1\\P1.esch2   ← "原"板
"""
import csv
import json
import sys
from collections import defaultdict

dec = json.JSONDecoder()


def _payload(rest):
    rest = rest.rstrip()
    if rest.endswith("|"):
        rest = rest[:-1]
    try:
        return dec.raw_decode(rest)[0]
    except json.JSONDecodeError:
        j = rest.rfind("}")
        if j > 0:
            try:
                return dec.raw_decode(rest[: j + 1])[0]
            except json.JSONDecodeError:
                return None
        return None


def load(path):
    """返回 [{type, id, p}]；无法解析的行被跳过。"""
    recs = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line.strip():
                continue
            i = line.find("||")
            if i < 0:
                continue
            try:
                head = dec.raw_decode(line[:i])[0]
            except json.JSONDecodeError:
                continue
            p = _payload(line[i + 2:])
            if isinstance(p, dict):
                recs.append({"type": head.get("type"), "id": head.get("id"), "p": p})
    return recs


def _sortkey(des):
    i = 0
    while i < len(des) and not des[i].isdigit():
        i += 1
    return (des[:i], int(des[i:] or 0), des)


def parse(path):
    """返回 (元件行列表, 网络名集合)。位号来自 ATTR(parentId=元件id, key=Designator)。"""
    recs = load(path)
    comps = {r["id"]: r["p"] for r in recs if r["type"] == "COMPONENT"}

    attrs = defaultdict(dict)          # parentId -> {key: value}
    for r in recs:
        pid, key = r["p"].get("parentId"), r["p"].get("key")
        if pid and key:
            attrs[pid].setdefault(key, r["p"].get("value"))

    rows = []
    for cid, cp in comps.items():
        a = attrs.get(cid, {})
        des = a.get("Designator", "")
        dev = a.get("Device", "") or cp.get("attrs", {}).get("DeviceName", "")
        if isinstance(dev, str) and dev.startswith("{"):
            try:
                dev = json.loads(dev).get("name", dev)
            except json.JSONDecodeError:
                pass
        rows.append({"des": des, "device": dev, "value": a.get("Value", ""),
                     "footprint": a.get("Footprint", ""),
                     "x": cp.get("x"), "y": cp.get("y"), "id": cid})

    nets = {str(r["p"].get("value")) for r in recs
            if r["type"] == "ATTR" and r["p"].get("key") in ("NET", "Global Net Name")
            and r["p"].get("value")}
    return rows, nets


def report(path):
    rows, nets = parse(path)
    comps = [r for r in rows if r["des"] and not r["des"].startswith("#PWR")]
    pwr = [r for r in rows if r["des"].startswith("#PWR")]
    nameless = [r for r in rows if not r["des"]]

    print(f"# 文件: {path}")
    print(f"# 元件总数 {len(rows)} ｜ 有位号(非电源符号) {len(comps)} ｜ "
          f"电源符号 {len(pwr)} ｜ 无位号 {len(nameless)} ｜ 网络名 {len(nets)}")
    print()
    hdr = f"{'位号':<9}{'器件':<38}{'值':<12}{'封装':<22}x,y"
    print(hdr)
    print("-" * len(hdr))
    for r in sorted(comps, key=lambda r: _sortkey(r["des"])):
        print(f"{r['des']:<9}{str(r['device'])[:36]:<38}{str(r['value'])[:10]:<12}"
              f"{str(r['footprint'])[:20]:<22}{r['x']},{r['y']}")
    print()
    uniq = sorted(nets)
    print(f"# 网络名 {len(uniq)} 个")
    for i in range(0, len(uniq), 6):
        print("   " + "".join(f"{x:<16}" for x in uniq[i:i + 6]))
    return comps, nets


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    comps, nets = report(path)

    if "--diff" in sys.argv:
        other = sys.argv[sys.argv.index("--diff") + 1]
        ocomps, onets = parse(other)
        here = {r["des"] for r in comps}
        there = {r["des"] for r in ocomps if r["des"] and not r["des"].startswith("#PWR")}
        print()
        print(f"=== 对照 {other} ===")
        miss = sorted(there - here, key=_sortkey)
        extra = sorted(here - there, key=_sortkey)
        print(f"对方有、本图【缺】的位号（{len(miss)}）：{' '.join(miss) or '（无）'}")
        print(f"本图有、对方没有的位号（{len(extra)}）：{' '.join(extra) or '（无）'}")
        dn = sorted(onets - nets)
        print(f"对方有、本图缺的网络名（{len(dn)}）：{' '.join(dn) or '（无）'}")

    if "--csv" in sys.argv:
        out = sys.argv[sys.argv.index("--csv") + 1]
        with open(out, "w", newline="", encoding="utf-8-sig") as fh:
            w = csv.DictWriter(fh, fieldnames=["des", "device", "value", "footprint", "x", "y", "id"])
            w.writeheader()
            w.writerows(sorted(comps, key=lambda r: _sortkey(r["des"])))
        print(f"\n# 已写出 {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
