#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""**重构安全审计**：证明一次"排版/结构整改"没有改掉任何非空白内容。

用法：
    python tools/fw_prep/audit_refactor.py [基线提交] [目标提交]
    # 默认： HEAD~1  HEAD

做法：对两个提交之间每个**文本**改动文件，取出新旧内容，去掉所有空白字符后比较：
  · 相同  → 该文件**只做了排版/缩进**改动，逻辑一字未变（可放心）
  · 不同  → 有真实内容改动，逐个列出来人工确认（例如"头文件守卫改名"是预期的）
并特别检查**关键文件**（控制中断、厂商库、FreeRTOS）里有没有出现真实改动。

为什么不用 `git diff -w`：`-w` 只忽略空白差异，会把"守卫改名""结构调整"这类
真实改动混在一起看不出来；这个脚本给的是**分类清单**，一眼看出哪些文件动过逻辑。

配套：`tools/fw_prep/normalize_whitespace.py`（只动空白的规范化器，自带安全阀）。
"""
import subprocess
import sys


def git(*args):
    r = subprocess.run(["git"] + list(args), capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0 and r.stderr:
        print(r.stderr.strip(), file=sys.stderr)
    return r.stdout


def squash(s):
    """去掉所有空白字符 —— 用于判断"是否只有排版变化"。"""
    return "".join(ch for ch in s if not ch.isspace())


CRITICAL_PREFIXES = (
    "firmware/App/Control/control.c",   # 5 ms/10 ms 控制中断：逻辑绝不能被碰
    "firmware/Drivers/FWLIB",           # 厂商标准外设库
    "firmware/Drivers/CMSIS",           # ARM/ST 内核与启动
    "firmware/Middlewares",             # FreeRTOS 官方内核
    "firmware/App/Sensor/DMP",          # InvenSense 官方 DMP
    "firmware/App/Comms/DataScope",     # 厂商波形协议
)


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else "HEAD~1"
    head = sys.argv[2] if len(sys.argv) > 2 else "HEAD"

    files = [f for f in git("diff", "--name-only", base, head).splitlines() if f]
    if not files:
        print(f"{base}..{head} 之间没有改动")
        return 0

    only_ws, real, binary = [], [], []
    for f in files:
        old = git("show", f"{base}:{f}")
        new = git("show", f"{head}:{f}")
        if "\x00" in old or "\x00" in new:
            binary.append(f)
        elif squash(old) == squash(new):
            only_ws.append(f)
        else:
            real.append(f)

    print(f"审计范围 {base}..{head}：改动文件 {len(files)} 个\n")
    print(f"① 纯排版/空白改动（去掉空白后逐字符相同）：{len(only_ws)} 个")
    for f in sorted(only_ws):
        print("   ", f)
    print(f"\n② 有真实内容改动（需逐条确认是预期）：{len(real)} 个")
    for f in sorted(real):
        print("   ", f)
    if binary:
        print(f"\n③ 二进制/含 NUL：{len(binary)} 个")
        for f in binary:
            print("   ", f)

    hits = [f for f in real + binary if f.startswith(CRITICAL_PREFIXES)]
    print("\n★ 关键文件（控制中断 / 厂商库 / 内核）里出现真实改动：",
          ("无 ✅" if not hits else ", ".join(hits)))
    return 0 if not hits else 1


if __name__ == "__main__":
    sys.exit(main())
