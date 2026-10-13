#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""一键跑完 B 阶段的**全部离线核验**（不需要板子）。

包含：
    1. 协议层原生单测（crc16 / protocol / ringbuf）
    2. 传感器算法原生单测（gas / dht11 / sensor_report）
    3. 巡检状态机原生单测（patrol）
    4. 固件交叉编译 + 向量表核验（读 .map 判归属）
    5. 引脚基线文件层核验（PCB 的 PAD_NET）

用法：
    python tools/fw_prep/run_all_tests.py            # 跑全部
    python tools/fw_prep/run_all_tests.py --no-build # 跳过交叉编译（更快）
"""
import os
import re
from toolchain import arm_tool
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FW = os.path.join(ROOT, "firmware")

TESTS = [
    ("协议层原生单测", ["run_proto_test.py"]),
    ("传感器算法原生单测", ["run_sensor_test.py"]),
    ("巡检状态机原生单测", ["run_patrol_test.py"]),
    ("PID 参数打包原生单测", ["run_param_test.py"]),
]


def run(cmd, cwd=None, capture=True):
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def main():
    skip_build = "--no-build" in sys.argv
    results = []

    for label, script in TESTS:
        path = os.path.join(ROOT, "tools", "fw_prep", script[0])
        rc, out = run([sys.executable, path])
        m = re.search(r"通过 (\d+) 条，失败 (\d+) 条", out)
        detail = f"通过 {m.group(1)} / 失败 {m.group(2)}" if m else "(未解析到结果)"
        results.append((label, rc == 0, detail))
        if rc != 0:
            print(f"--- {label} 输出（失败）---")
            print(out)

    if not skip_build:
        rc, out = run(["make"], cwd=FW)
        # 占用从 .elf 直接量（`make` 增量构建时不会重复打印 memory usage）
        detail = "(未生成 elf)"
        elf = os.path.join(FW, "build", "MiniBalance.elf")
        if rc == 0 and os.path.exists(elf):
            rc_sz, out_sz = run([arm_tool("size") or "arm-none-eabi-size", elf])
            m = re.search(r"^\s*(\d+)\s+(\d+)\s+(\d+)", out_sz, re.M)
            if m:
                detail = (f"text {m.group(1)} B + data {m.group(2)} B "
                          f"= Flash {int(m.group(1)) + int(m.group(2))} B, "
                          f"bss {m.group(3)} B")
        results.append(("固件交叉编译", rc == 0, detail))

        if rc == 0:
            rc2, out2 = run([sys.executable,
                             os.path.join(ROOT, "tools", "fw_prep", "check_vector_table.py")])
            results.append(("镜像向量表核验", rc2 == 0,
                            "通过" if rc2 == 0 else "见输出"))
            if rc2 != 0:
                print(out2)

            rc4, out4 = run([sys.executable,
                             os.path.join(ROOT, "tools", "fw_prep", "check_isr_safety.py")])
            results.append(("中断安全（不得调 RTOS API）", rc4 == 0,
                            "通过" if rc4 == 0 else "见输出"))
            if rc4 != 0:
                print(out4)

    rc3, out3 = run([sys.executable, os.path.join(ROOT, "tools", "fw_prep", "pin_check.py")])
    m3 = re.search(r"不符 (\d+) 个", out3)
    results.append(("引脚基线核验（PCB 文件层）", rc3 == 0,
                    f"不符 {m3.group(1)} 个" if m3 else "(未解析)"))

    print("\n================ B 阶段离线核验汇总 ================")
    bad = 0
    for label, good, detail in results:
        print(f"  [{'OK  ' if good else 'FAIL'}] {label:<24} {detail}")
        if not good:
            bad += 1
    print("====================================================")
    print("结论：" + ("全部通过 ✅" if bad == 0 else f"{bad} 项未通过 ❌"))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
