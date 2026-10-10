#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 CMSIS 头文件生成 **GCC 版**启动文件（向量表 + 弱定义中断处理函数）。

为什么生成而不是手写：
    基线自带的 `startup_stm32f40_41xxx.s` 是 **Keil(ARMCC) 语法**，GCC 汇编器不接受；
    手抄 82 个中断向量极易打错名字（错了不报错，只是中断进了默认死循环）。
    本脚本直接从 `User/stm32f4xx.h` 的 **IRQn 枚举**里取名，顺序与编号由 CMSIS 保证。

用法：
    python tools/fw_prep/gen_startup.py          # 生成到 firmware/Drivers/CMSIS/startup_stm32f405.S
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HDR = os.path.join(ROOT, "firmware", "User", "stm32f4xx.h")
OUT = os.path.join(ROOT, "firmware", "Drivers", "CMSIS", "startup_stm32f405.S")

CORE_VECTORS = [
    ("g_pfnVectors", None), ("Reset_Handler", None),
    ("NMI_Handler", None), ("HardFault_Handler", None),
    ("MemManage_Handler", None), ("BusFault_Handler", None),
    ("UsageFault_Handler", None), ("0", None), ("0", None), ("0", None), ("0", None),
    ("SVC_Handler", None), ("DebugMon_Handler", None), ("0", None),
    ("PendSV_Handler", None), ("SysTick_Handler", None),
]


def parse_irqs(path):
    """抽出 STM32F40_41xxx 变体的全部外设中断（IRQn >= 0），返回 [(name, num)]。"""
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    start = next(i for i, l in enumerate(lines) if "typedef enum IRQn" in l)
    stop = next(i for i, l in enumerate(lines)
                if i > start and re.search(r"#endif\s*/\*\s*STM32F40_41xxx", l))
    irqs = {}
    for l in lines[start:stop + 1]:
        m = re.match(r"\s*([A-Za-z0-9_]+_IRQn)\s*=\s*(\d+)\s*(?:,|/\*|$)", l)
        if m:
            irqs[int(m.group(2))] = m.group(1)
    missing = [n for n in range(max(irqs) + 1) if n not in irqs]
    if missing:
        raise SystemExit(f"IRQn 编号不连续，缺 {missing}")
    return [(irqs[n], n) for n in range(max(irqs) + 1)]


def main():
    irqs = parse_irqs(HDR)
    print(f"从 {os.path.relpath(HDR, ROOT)} 解析到外设中断 {len(irqs)} 个"
          f"（IRQn 0~{irqs[-1][1]}）")

    out = []
    out.append("/* 自动生成，请勿手改 —— 由 tools/fw_prep/gen_startup.py 从 User/stm32f4xx.h 的 IRQn 生成 */")
    out.append("/* STM32F405RGT6 / Cortex-M4F / GCC(arm-none-eabi) 向量表与启动代码 */")
    out.append("    .syntax unified")
    out.append("    .cpu cortex-m4")
    out.append("    .fpu softvfp")
    out.append("    .thumb")
    out.append("")
    out.append("    .global g_pfnVectors")
    out.append("    .global Reset_Handler")
    out.append("")
    out.append("    .section .isr_vector,\"a\",%progbits")
    out.append("    .type g_pfnVectors, %object")
    out.append("g_pfnVectors:")
    out.append("    .word _estack")
    for name, _ in CORE_VECTORS[1:]:
        out.append(f"    .word {name if name != '0' else '0'}")
    for name, num in irqs:
        out.append(f"    .word {name[:-5]}_IRQHandler" if name.endswith("_IRQn") else f"    .word {name}")
    out.append("    .size g_pfnVectors, .-g_pfnVectors")
    out.append("")
    out.append("    .section .text.Reset_Handler,\"ax\",%progbits")
    out.append("    .weak Reset_Handler")
    out.append("    .type Reset_Handler, %function")
    out.append("Reset_Handler:")
    out.append("    ldr   sp, =_estack          /* 设置栈顶 */")
    out.append("    bl    SystemInit            /* 时钟、FPU、向量表偏移 */")
    out.append("    ldr   r0, =_sidata          /* .data 段从 Flash 搬到 RAM */")
    out.append("    ldr   r1, =_sdata")
    out.append("    ldr   r2, =_edata")
    out.append("    b     2f")
    out.append("1:  ldr   r3, [r0], #4")
    out.append("    str   r3, [r1], #4")
    out.append("2:  cmp   r1, r2")
    out.append("    bcc   1b")
    out.append("    ldr   r1, =_sbss            /* .bss 清零 */")
    out.append("    ldr   r2, =_ebss")
    out.append("    movs  r3, #0")
    out.append("    b     4f")
    out.append("3:  str   r3, [r1], #4")
    out.append("4:  cmp   r1, r2")
    out.append("    bcc   3b")
    out.append("    bl    __libc_init_array     /* C 运行时初始化（newlib） */")
    out.append("    bl    main")
    out.append("5:  b     5b")
    out.append("    .size Reset_Handler, .-Reset_Handler")
    out.append("")
    out.append("/* 未实现的中断统一进这里停住（便于调试定位） */")
    out.append("    .section .text.Default_Handler,\"ax\",%progbits")
    out.append("    .type Default_Handler, %function")
    out.append("Default_Handler:")
    out.append("    b     Default_Handler")
    out.append("    .size Default_Handler, .-Default_Handler")
    out.append("")
    # 弱定义：核心异常 + 全部外设中断
    core_exc = [n for n, _ in CORE_VECTORS if n.endswith("_Handler")]
    out.append("/* 核心异常（若 C 代码里定义了同名函数，则覆盖这里） */")
    for name in core_exc:
        if name == "Reset_Handler":
            continue
        out.append(f"    .weak {name}")
        out.append(f"    .thumb_set {name}, Default_Handler")
    out.append("")
    out.append("/* 外设中断（名字与 IRQn 一一对应，由 CMSIS 头保证） */")
    for name, _ in irqs:
        h = name[:-5] + "_IRQHandler" if name.endswith("_IRQn") else name
        out.append(f"    .weak {h}")
        out.append(f"    .thumb_set {h}, Default_Handler")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(out) + "\n")
    print(f"已写出 {os.path.relpath(OUT, ROOT)}（向量 {len(CORE_VECTORS) + len(irqs)} 项）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
