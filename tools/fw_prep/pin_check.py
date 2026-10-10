#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""B 阶段引脚基线核验（**文件层独立方法**，EDA 可关闭）。

为什么要单独做这一份：
    `AGENTS.md` 铁律 23 —— **不要用同一套假设去验自己写的东西**。
    B 的引脚基线此前是从"原理图文档 + 交接说明"里抄来的；
    本脚本改从 **PCB 文件（`.epcb2`）** 里读 `PAD_NET` 记录（`组件 id + 焊盘号 + 网络名`），
    与期望表逐条比对。数据来源不同（PCB 而非原理图）、表示形式不同（焊盘网络而非网名标签），
    因此是一次真正的交叉验证。

用法：
    # 按期望表核验（默认用完工快照的 PCB2）
    python tools/fw_prep/pin_check.py

    # 指定文件 / 只看某些位号 / 打印全部
    python tools/fw_prep/pin_check.py --pcb <PCB2.epcb2> --only J13,J10
    python tools/fw_prep/pin_check.py --dump
"""
import argparse
import collections
import json
import os
import re
import sys

DEFAULT_PCB = (r"D:\260921Task\hardware\硬件工程_完工版_20261008_最终"
               r"\pcb\PCB2.epcb2")

# 电源网名规范化：PCB 侧带 POWER_ 前缀
NET_ALIAS = {
    "POWER_GNDREF": "GND",
    "POWER_+5V": "+5V",
    "POWER_+3V3": "+3.3V",
    "POWER_+3.3V": "+3.3V",
    "POWER_+12V": "+12V",
    "": "(空)",
}

# ★ B 阶段关心的引脚基线（来源：AGENTS.md §7 + 交接说明 §2.1，逐脚核对用）
EXPECTED = {
    "J13": {1: "MQ2_AO", 2: "PB3", 3: "GND", 4: "+5V"},          # MQ-2
    "J10": {1: "MQ135_AO", 2: "PC3", 3: "GND", 4: "+5V"},        # MQ-135
    "J9":  {1: "GND", 2: "+3.3V", 3: "PA7"},                     # DHT11
    "J5":  {1: "+5V", 2: "PC4", 3: "PB0", 4: "GND"},             # HC-SR04
    "J7":  {1: "(空)", 2: "PA2", 3: "PA3", 4: "GND", 5: "+5V", 6: "(空)"},  # 蓝牙
    "J18": {1: "+5V", 2: "PC5", 3: "GND"},                       # 蜂鸣器
    "J11": {1: "+5V", 2: "GND", 3: "PB12"},                      # 红外 L（DNP）
    "J14": {1: "+5V", 2: "GND", 3: "PC1"},                       # 红外 R（DNP）
    # 电阻网络：分压 / 上拉（两端网络必须落在这些组合里，顺序不限）
    "R10": {1: "MQ2_AO", 2: "PA5"},
    "R11": {1: "PA5", 2: "GND"},
    "R12": {1: "MQ135_AO", 2: "PA6"},
    "R13": {1: "PA6", 2: "GND"},
    "R14": {1: "PA7", 2: "+3.3V"},
}

# 不做逐脚顺序断言、只做"两端网络集合"比对的位号（电阻两端可互换）
UNORDERED = {"R10", "R11", "R12", "R13", "R14"}


def load(path):
    """解析 `.epcb2`：返回 [{type, id, p}]（与 hardware/sch_netlist.py 同规则）。"""
    dec = json.JSONDecoder()
    recs = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            i = line.find("||")
            if i < 0:
                continue
            try:
                head = dec.raw_decode(line[:i])[0]
            except json.JSONDecodeError:
                continue
            rest = line[i + 2:].rstrip()
            if rest.endswith("|"):
                rest = rest[:-1]
            try:
                p = dec.raw_decode(rest)[0]
            except json.JSONDecodeError:
                continue
            if isinstance(p, dict):
                recs.append({"type": head.get("type"), "id": head.get("id"), "p": p})
    return recs


def norm(net):
    net = "" if net is None else str(net)
    return NET_ALIAS.get(net, net)


def extract(path):
    """返回 {位号: {焊盘号: 规范化网络名}}。"""
    recs = load(path)

    desig = {}
    for r in recs:
        if r["type"] == "ATTR" and r["p"].get("key") == "Designator":
            desig.setdefault(str(r["p"].get("parentId")), r["p"].get("value"))

    pads = collections.defaultdict(dict)
    for r in recs:
        if r["type"] != "PAD_NET":
            continue
        # id 形如 ["PAD_NET", "<componentId>", "<padNumber>", "<padPrimitiveId>"]
        try:
            _, cid, num, _pad = json.loads(r["id"])
        except (ValueError, TypeError):
            continue
        d = desig.get(str(cid))
        if not d:
            continue
        try:
            num = int(re.match(r"\d+", str(num)).group())
        except AttributeError:
            continue
        pads[d][num] = norm(r["p"].get("padNet"))
    return pads


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pcb", default=DEFAULT_PCB)
    ap.add_argument("--only", default="")
    ap.add_argument("--dump", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.pcb):
        print(f"找不到文件：{args.pcb}")
        return 2

    pads = extract(args.pcb)
    print(f"# 文件：{args.pcb}")
    print(f"# 解析到位号 {len(pads)} 个；PAD_NET 记录 "
          f"{sum(len(v) for v in pads.values())} 条\n")

    if args.dump:
        for d in sorted(pads, key=lambda s: (re.match(r"[A-Za-z]+", s).group(), int(re.sub(r"\D", "", s) or 0))):
            items = "  ".join(f"{n}:{pads[d][n]}" for n in sorted(pads[d]))
            print(f"{d:<6} {items}")
        return 0

    only = {x.strip() for x in args.only.split(",") if x.strip()}
    targets = only or set(EXPECTED)
    bad = 0
    for d in sorted(targets):
        got = pads.get(d, {})
        if d not in pads:
            print(f"[缺失] {d}：PCB 里没有这个位号")
            bad += 1
            continue
        exp = EXPECTED[d]
        if d in UNORDERED:
            ok = sorted(got.values()) == sorted(exp.values())
            detail = f"期望两端 {{{' , '.join(sorted(exp.values()))}}}  实得 {{{' , '.join(sorted(got.values()))}}}"
        else:
            ok = got == exp
            detail = "期望 " + " ".join(f"{n}={exp[n]}" for n in sorted(exp)) + \
                     "\n             实得 " + " ".join(f"{n}={got.get(n, '——')}" for n in sorted(exp))
        print(f"[{'OK  ' if ok else '不符'}] {d}\n       {detail}")
        if not ok:
            bad += 1
    print()
    print(f"# 结论：核验 {len(targets)} 个位号，**不符 {bad} 个**"
          + ("（引脚基线与文档一致 ✅）" if bad == 0 else "（需查清后再写固件 ❗）"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
