#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在 PC 上原生编译并运行固件的协议单元测试（**不需要板子**）。

做三件事：
    1. 确保测试向量是最新的（跑 gen_test_vectors.py）
    2. 用 PC 上的 C 编译器（gcc/clang，自动探测）把固件的协议模块 + 测试程序编译成 PC 可执行文件
       （协议/crc/环形缓冲三个模块刻意只依赖 <stdint.h>，所以能脱离 STM32 编译）
    3. 运行并汇总结果

用法：
    python tools/fw_prep/run_proto_test.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FW = os.path.join(ROOT, "firmware")
OUT = os.path.join(ROOT, "tools", "fw_prep", "out")
TSV = os.path.join(ROOT, "docs", "protocol", "test_vectors.tsv")

# PC 端 C 编译器：由 toolchain.py 自动探测（PATH / FW_HOST_CC / Qt 自带的 MinGW），
# **不写死任何安装路径** —— 换台机器只要求 gcc 在 PATH 上，或设 FW_HOST_CC。
from toolchain import host_cc, child_env

SOURCES = [
    os.path.join(FW, "tests", "proto_test.c"),
    os.path.join(FW, "App", "Comms", "protocol.c"),
    os.path.join(FW, "App", "Comms", "crc16.c"),
    os.path.join(FW, "App", "Comms", "ringbuf.c"),
]


def find_cc():
    """返回 PC 端 C 编译器路径（见 toolchain.py 的查找顺序）。"""
    return host_cc()


def main():
    cc = find_cc()
    if cc is None:
        print("未找到 PC 端 C 编译器：请把 gcc/clang 加入 PATH，或用环境变量 FW_HOST_CC 指定（见 tools/fw_prep/toolchain.py）")
        return 2

    # 1) 刷新向量
    gen = os.path.join(ROOT, "tools", "fw_prep", "gen_test_vectors.py")
    subprocess.run([sys.executable, gen], stdout=subprocess.DEVNULL)

    if not os.path.exists(TSV):
        print(f"缺少测试向量 {TSV}")
        return 2

    os.makedirs(OUT, exist_ok=True)
    exe = os.path.join(OUT, "proto_test.exe")
    cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-O1", "-g",
           "-I", os.path.join(FW, "App", "Comms")] + SOURCES + ["-o", exe]
    print("编译：", " ".join(os.path.relpath(a, ROOT) if os.path.isabs(a) else a
                            for a in cmd))
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       env=child_env(cc))
    out = p.stdout.decode("utf-8", "replace")
    if p.returncode != 0:
        print("=== 编译失败 ===")
        print(out)
        return 1
    warns = [l for l in out.splitlines() if "warning:" in l]
    print(f"编译成功（警告 {len(warns)} 条）")
    for w in warns:
        print("  ", w.strip())

    # 3) 运行
    p = subprocess.run([exe, TSV], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(p.stdout.decode("utf-8", "replace"))
    m = re.search(r"通过 (\d+) 条，失败 (\d+) 条", p.stdout.decode("utf-8", "replace"))
    if m:
        print(f"→ 失败 {m.group(2)} 条，退出码 {p.returncode}")
    return p.returncode


if __name__ == "__main__":
    sys.exit(main())
