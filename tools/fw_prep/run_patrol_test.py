#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在 PC 上原生编译并运行**巡检状态机**单元测试（不需要板子）。

状态机用脚本化时间线验证（完整任务链 + 三条安全通道 + 上报），
见 firmware/tests/patrol_test.c 顶部说明。

用法：
    python tools/fw_prep/run_patrol_test.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FW = os.path.join(ROOT, "firmware")
OUT = os.path.join(ROOT, "tools", "fw_prep", "out")

CANDIDATES = [
    r"D:\0_APP\QT\Tools\mingw1310_64\bin\gcc.exe",
    "gcc",
    "cc",
]

SOURCES = [
    os.path.join(FW, "tests", "patrol_test.c"),
    os.path.join(FW, "App", "Task", "patrol.c"),
]

INCLUDES = [
    os.path.join(FW, "App", "Task"),
    os.path.join(FW, "App", "Comms"),
]


def find_cc():
    for c in CANDIDATES:
        try:
            p = subprocess.run([c, "--version"], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
            if p.returncode == 0:
                return c
        except (OSError, subprocess.SubprocessError):
            continue
    return None


def main():
    cc = find_cc()
    if cc is None:
        print("找不到 PC 端 C 编译器（试过 MinGW gcc / gcc / cc）")
        return 2

    os.makedirs(OUT, exist_ok=True)
    exe = os.path.join(OUT, "patrol_test.exe")
    cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-O1", "-g"]
    for inc in INCLUDES:
        cmd += ["-I", inc]
    cmd += SOURCES + ["-o", exe]
    print("编译：", " ".join(os.path.relpath(a, ROOT) if os.path.isabs(a) else a
                            for a in cmd))
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = p.stdout.decode("utf-8", "replace")
    if p.returncode != 0:
        print("=== 编译失败 ===")
        print(out)
        return 1
    warns = [l for l in out.splitlines() if "warning:" in l]
    print(f"编译成功（警告 {len(warns)} 条）")
    for w in warns:
        print("  ", w.strip())

    p = subprocess.run([exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = p.stdout.decode("utf-8", "replace")
    print(text)
    m = re.search(r"通过 (\d+) 条，失败 (\d+) 条", text)
    if m:
        print(f"→ 失败 {m.group(2)} 条，退出码 {p.returncode}")
    return p.returncode


if __name__ == "__main__":
    sys.exit(main())
