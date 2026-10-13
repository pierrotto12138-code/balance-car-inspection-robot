#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""**中断安全静态检查**：确认中断服务函数里没有调用 FreeRTOS API。

为什么必须检查（决策 37/42）：
    本项目两个中断的抢占优先级都比 configMAX_SYSCALL_INTERRUPT_PRIORITY(5) 更"高"
    （数值更小）：
        EXTI15_10（控制中断，5 ms/10 ms 命脉）  = 1
        USART2（蓝牙）                          = 0
    这种中断**运行在中断屏蔽阈值之上**，在里面调用任何 FreeRTOS API
    （xQueueSend / xSemaphoreTake / vTaskDelay / taskENTER_CRITICAL…）
    都会破坏内核状态，而且**编译器不会报错、上板才炸**。
    所以这里用源码静态检查把它挡住，作为每次提交前的自检项。

做法：
    1. 从 `firmware/build/*.map` 或 `check_vector_table.py` 同样的信息来源，
       取出向量表里实际用到的中断处理函数名（谁在实现由 .map 判定）；
    2. 在源码里定位这些函数的**函数体**（大括号配对）；
    3. 在大括号内搜索 FreeRTOS API 名单，命中即报错。

用法：
    python tools/fw_prep/check_isr_safety.py
"""
import os
import re
from toolchain import arm_tool
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FW = os.path.join(ROOT, "firmware")

# FreeRTOS 里"绝不能在高于 syscall 阈值的中断里调用"的 API（按前缀/名字匹配）
FORBIDDEN = [
    r"\bxQueueSend\w*", r"\bxQueueReceive\w*", r"\bxQueueGenericSend\w*",
    r"\bxSemaphoreTake\w*", r"\bxSemaphoreGive\w*", r"\bxTaskNotify\w*",
    r"\bvTaskDelay\w*", r"\bvTaskSuspend\w*", r"\bvTaskResume\w*",
    r"\bxTaskCreate\w*", r"\bvTaskStartScheduler\w*", r"\bxTimer\w+",
    r"\btaskENTER_CRITICAL\b", r"\btaskEXIT_CRITICAL\b",
    r"\btaskYIELD\b", r"\bportYIELD_FROM_ISR\b", r"\bvPortFree\b", r"\bpvPortMalloc\b",
    r"\buxSemaphoreGetCount\b", r"\bxEventGroup\w*", r"\bxStreamBuffer\w*",
]

# 允许的形式：FromISR 变体本身是"合法用法"，但本项目约定**一个都不用**（保持时序简单）
FROM_ISR = re.compile(r"\w+FromISR\b")


def isr_names():
    """从 .map 取出实际参与链接的中断处理函数（与 check_vector_table 同源）。"""
    elf = os.path.join(FW, "build", "MiniBalance.elf")
    mp = os.path.join(FW, "build", "MiniBalance.map")
    name_by_addr = {}
    # nm 给出符号地址（交叉工具链由 toolchain.py 探测）
    _nm = arm_tool("nm") or "arm-none-eabi-nm"
    try:
        out = subprocess.run([_nm, "-n", elf], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT).stdout.decode("utf-8", "replace")
    except OSError:
        return {}
    for line in out.splitlines():
        m = re.match(r"([0-9a-fA-F]{8})\s+\w\s+(\S+)", line)
        if m and m.group(2).endswith("_IRQHandler"):
            name_by_addr[m.group(2)] = int(m.group(1), 16)

    # map 判定归属
    owner = {}
    if os.path.exists(mp):
        lines = open(mp, encoding="utf-8", errors="replace").read().splitlines()
        for i, line in enumerate(lines):
            m = re.match(r"\s*\.text\.(\w+_IRQHandler)", line)
            if m and i + 1 < len(lines):
                m2 = re.search(r"0x[0-9a-f]+\s+0x[0-9a-f]+\s+(\S+\.o)", lines[i + 1])
                if m2:
                    owner[m.group(1)] = os.path.basename(m2.group(1))
    return {n: owner.get(n, "?") for n in name_by_addr if owner.get(n, "?").endswith(".o")}


def body_of(src, func):
    """粗匹配函数体（按大括号配对）。允许 `)` 与 `{` 之间夹注释。"""
    m = re.search(r"\b" + re.escape(func) + r"\s*\([^)]*\)[^;{]*\{", src)
    if not m:
        return None
    i = m.end()
    depth = 1
    while i < len(src) and depth > 0:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
        i += 1
    return src[m.end():i]


def main():
    isrs = isr_names()
    if not isrs:
        print("取不到中断处理函数（先 make 出 build/MiniBalance.elf 与 .map）")
        return 2

    sources = []
    for sub in ("Drivers", "System", "App", "User"):
        for dirpath, _dirs, files in os.walk(os.path.join(FW, sub)):
            for f in files:
                if f.endswith((".c", ".C")):
                    sources.append(os.path.join(dirpath, f))

    print(f"# 参与链接的中断处理函数 {len(isrs)} 个：")
    for n, o in sorted(isrs.items()):
        print(f"    {n:<32} ← {o}")

    bad = 0
    print("\n# 逐个检查函数体内是否出现 FreeRTOS API")
    for name, obj in sorted(isrs.items()):
        found_any = False
        for src_path in sources:
            src = open(src_path, encoding="utf-8", errors="replace").read()
            body = body_of(src, name)
            if body is None:
                continue
            found_any = True
            hits = []
            for pat in FORBIDDEN:
                hits += re.findall(pat, body)
            hits += [h for h in FROM_ISR.findall(body)]
            rel = os.path.relpath(src_path, ROOT)
            if hits:
                bad += 1
                print(f"  [违规] {name}（{rel}）调用了：{', '.join(sorted(set(hits)))}")
            else:
                print(f"  [OK  ] {name:<32}（{rel}）无 FreeRTOS API")
        if not found_any:
            print(f"  [??  ] {name}：源码里没找到函数体（可能在头文件/汇编里）")

    print("\n# 中断抢占优先级 vs FreeRTOS 的 syscall 阈值")
    thr = None
    cfgp = os.path.join(FW, "User", "FreeRTOSConfig.h")
    if os.path.exists(cfgp):
        m = re.search(r"configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY\s+(\d+)",
                      open(cfgp, encoding="utf-8", errors="replace").read())
        if m:
            thr = int(m.group(1))
    print(f"  configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = {thr}")

    prio = {}
    for src_path in sources:
        src = open(src_path, encoding="utf-8", errors="replace").read()
        for m in re.finditer(r"NVIC_IRQChannel\s*=\s*(\w+)\s*;(?:.{0,400}?)"
                             r"NVIC_IRQChannelPreemptionPriority\s*=\s*(0x[0-9a-fA-F]+|\d+)",
                             src, re.S):
            irq = m.group(1)
            try:
                p = int(m.group(2), 0)
            except ValueError:
                continue
            prio[irq] = (p, os.path.relpath(src_path, ROOT))
    for irq, (p, rel) in sorted(prio.items(), key=lambda kv: kv[1][0]):
        above = (thr is not None and p < thr)
        tag = ("高于阈值 → 绝不能调用 RTOS API（含 FromISR）" if above
               else "低于阈值 → 允许调用 ...FromISR() 系列")
        print(f"  {irq:<24} 抢占优先级 {p}  ({rel})  {tag}")

    print("\n结论：" + ("所有中断都不调用 FreeRTOS API ✅" if bad == 0
                       else f"发现 {bad} 处违规 ❌（高于 syscall 阈值的中断里禁止调用 RTOS API）"))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
