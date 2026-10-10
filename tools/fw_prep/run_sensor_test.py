#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在 PC 上原生编译并运行**传感器算法**单元测试（不需要板子）。

流程与 run_proto_test.py 相同：
    1. 用 Python 独立实现重新生成 sensor_vectors.csv
    2. 用本机 MinGW gcc 编译 gas.c / dht11.c（纯算法层）+ tests/sensor_test.c
    3. 运行并汇总

用法：
    python tools/fw_prep/run_sensor_test.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FW = os.path.join(ROOT, "firmware")
OUT = os.path.join(ROOT, "tools", "fw_prep", "out")
CSV = os.path.join(FW, "tests", "sensor_vectors.csv")

CANDIDATES = [
    r"D:\0_APP\QT\Tools\mingw1310_64\bin\gcc.exe",
    "gcc",
    "cc",
]

SOURCES = [
    os.path.join(FW, "tests", "sensor_test.c"),
    os.path.join(FW, "App", "Sensor", "gas.c"),
    os.path.join(FW, "App", "Sensor", "dht11.c"),
    os.path.join(FW, "App", "Sensor", "sensor_report.c"),
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

    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "fw_prep",
                                                 "gen_sensor_vectors.py")],
                   stdout=subprocess.DEVNULL)
    if not os.path.exists(CSV):
        print(f"缺少测试向量 {CSV}")
        return 2

    os.makedirs(OUT, exist_ok=True)
    exe = os.path.join(OUT, "sensor_test.exe")
    cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-O1", "-g",
           "-I", os.path.join(FW, "App", "Sensor"), "-I", os.path.join(FW, "App", "Comms")] + SOURCES + ["-o", exe, "-lm"]
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

    p = subprocess.run([exe, CSV], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = p.stdout.decode("utf-8", "replace")
    print(text)
    m = re.search(r"通过 (\d+) 条，失败 (\d+) 条", text)
    if m:
        print(f"→ 失败 {m.group(2)} 条，退出码 {p.returncode}")
    return p.returncode


if __name__ == "__main__":
    sys.exit(main())
