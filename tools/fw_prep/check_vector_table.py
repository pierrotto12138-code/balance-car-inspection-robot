#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""离线核验固件镜像：**向量表 + 入口 + 关键中断**（不需要板子）。

为什么必须单独验：
    向量表错一格，中断就永远不触发或者跳进 Default_Handler 死循环，
    而这种错误**编译不报、链接不报**，只有上板才发现。
    本脚本直接读 `.elf`（用 objdump/nm），逐条核对：
      1. 第 0 项 = 栈顶（应为 SRAM 顶 0x20020000）
      2. 第 1 项 = Reset_Handler（且带 thumb 位、落在 Flash 内）
      3. 每一条中断向量：要么是 0、要么是我们代码里定义的真正处理函数、
         要么是 Default_Handler（未使用的中断，允许）
      4. **重点检查**：项目实际要用的中断必须指向真函数（不能落到 Default_Handler）
      5. 入口点、`.isr_vector` 是否落在 0x08000000

用法：
    python tools/fw_prep/check_vector_table.py [firmware/build/MiniBalance.elf]
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ELF = sys.argv[1] if len(sys.argv) > 1 else \
    os.path.join(ROOT, "firmware", "build", "MiniBalance.elf")

OBJDUMP = "arm-none-eabi-objdump"
NM = "arm-none-eabi-nm"
READELF = "arm-none-eabi-readelf"

# 本项目实际依赖的中断（必须指向真处理函数）
REQUIRED_IRQS = {
    "EXTI15_10_IRQHandler": "MPU6050 INT → 5ms/10ms 控制节拍（命脉）",
    "USART2_IRQHandler": "蓝牙下行（协议解帧）",
    "TIM3_IRQHandler": "超声波 ECHO 输入捕获（PB0）",
    "TIM8_CC_IRQHandler": "航模遥控 CH1/CH2 捕获（PC6/PC7）",
    "TIM8_UP_TIM13_IRQHandler": "TIM8 更新中断",
    "SysTick_Handler": "FreeRTOS 节拍（P4 起需要）",
    "PendSV_Handler": "FreeRTOS 任务切换",
    "SVC_Handler": "FreeRTOS 启动第一个任务",
    "HardFault_Handler": "硬件错误（基线未自定义 → 允许是默认处理）",
}


def run(cmd):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.decode(
        "utf-8", "replace")


def main():
    if not os.path.exists(ELF):
        print(f"找不到 {ELF}，先 make")
        return 2

    # --- 符号表 ---
    syms = {}
    for line in run([NM, "-n", ELF]).splitlines():
        m = re.match(r"([0-9a-fA-F]{8})\s+(\w)\s+(\S+)", line)
        if m:
            syms[m.group(3)] = int(m.group(1), 16)

    # --- 段表 / 入口 ---
    hdr = run([READELF, "-h", ELF])
    entry = int(re.search(r"Entry point address:\s+0x([0-9a-fA-F]+)", hdr).group(1), 16)
    sections = {}
    for line in run([READELF, "-S", "-W", ELF]).splitlines():
        m = re.search(r"\]\s+(\S+)\s+\S+\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+([0-9a-f]{8})", line)
        if m:
            sections[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16))

    # --- 向量表原始内容 ---
    # objdump -s 的行格式是：" 08000000 00000220 41000008 ..."（无冒号，且按字节序显示）
    # 所以每个 8 位十六进制组要按**小端**还原成 32 位字。
    dump = run([OBJDUMP, "-s", "-j", ".isr_vector", ELF])
    words = []
    for line in dump.splitlines():
        m = re.match(r"\s+([0-9a-f]{1,8})\s+((?:[0-9a-f]{8}\s+){1,4})", line)
        if m:
            for grp in m.group(2).split():
                words.append(int.from_bytes(bytes.fromhex(grp), "little"))

    ok = True
    print(f"# 镜像：{os.path.relpath(ELF, ROOT)}（{os.path.getsize(ELF)} B）")
    isv = sections.get(".isr_vector")
    if isv:
        print(f"# .isr_vector: 地址 0x{isv[0]:08X}  大小 {isv[2]} B（{isv[2] // 4} 项）")
        if isv[0] != 0x08000000:
            print(f"  ❌ 向量表不在 0x08000000，而在 0x{isv[0]:08X} —— 芯片启动不了")
            ok = False
        else:
            print("  ✅ 向量表位于 0x08000000")
    print(f"# 入口点：0x{entry:08X}")
    # ARM 的入口点会带 thumb 位（最低位 = 1），比较时要屏蔽掉
    if (entry & ~1) != syms.get("Reset_Handler"):
        print(f"  ❌ 入口点与 Reset_Handler(0x{syms.get('Reset_Handler', 0):08X}) 不一致")
        ok = False
    else:
        print("  ✅ 入口点 = Reset_Handler（含 thumb 位）")

    if len(words) < 16:
        print(f"  ❌ 向量表读取失败：只解析到 {len(words)} 个字")
        return 1
    sp, reset = words[0], words[1]
    if sp != 0x20020000:
        print(f"  ❌ 栈顶 = 0x{sp:08X}，期望 0x20020000（SRAM 顶）")
        ok = False
    else:
        print("  ✅ 栈顶 = 0x20020000（SRAM 顶）")
    if reset != syms.get("Reset_Handler", 0) | 1:
        print(f"  ❌ Reset 向量 = 0x{reset:08X}，与 Reset_Handler(0x{syms.get('Reset_Handler', 0):08X}) 不符")
        ok = False
    else:
        print("  ✅ Reset 向量 = Reset_Handler（含 thumb 位）")

    # --- 逐条中断向量 ---
    names = list(syms)
    addr2name = {}
    for n in names:
        addr2name.setdefault(syms[n], []).append(n)
    default = syms.get("Default_Handler", 0)

    print(f"\n# 中断向量逐条核对（共 {len(words) - 16} 条外设中断）")
    real, defc, zero = 0, 0, 0
    required_state = {}
    for i, w in enumerate(words[16:], start=0):
        handler = f"IRQ{i}"
        if w == 0:
            zero += 1
            continue
        if w & ~1 == default:
            defc += 1
            continue
        real += 1
        for nm_ in addr2name.get(w & ~1, []):
            if nm_.endswith("_IRQHandler") and nm_ != "Default_Handler":
                handler = nm_
    print(f"  真处理函数 {real} 条 ｜ 落到 Default_Handler {defc} 条 ｜ 0（保留） {zero} 条")

    # 头 16 项里的核心异常
    core_names = ["g_pfnVectors", "Reset_Handler", "NMI_Handler", "HardFault_Handler",
                  "MemManage_Handler", "BusFault_Handler", "UsageFault_Handler",
                  None, None, None, None, "SVC_Handler", "DebugMon_Handler", None,
                  "PendSV_Handler", "SysTick_Handler"]
    print("\n# 关键处理函数是否指向真实实现")
    # 判断"处理函数究竟由哪个目标文件定义"最可靠的办法是读链接器的 **.map**：
    #   .text.SVC_Handler
    #                  0x08007ac0       0x24 build/port.o
    # 这样既能识别 ST 模板的空实现，又能确认 FreeRTOS 的 port.o 真的接上了。
    mapfile = os.path.splitext(ELF)[0] + ".map"
    owner = {}
    if os.path.exists(mapfile):
        lines = open(mapfile, encoding="utf-8", errors="replace").read().splitlines()
        for i, line in enumerate(lines):
            m = re.match(r"\s*\.text\.(\w+)", line)
            if m and i + 1 < len(lines):
                m2 = re.search(r"0x[0-9a-f]+\s+0x[0-9a-f]+\s+(\S+\.o)", lines[i + 1])
                if m2:
                    owner[m.group(1)] = os.path.basename(m2.group(1))
        print(f"  （已读 {os.path.basename(mapfile)} 判定符号归属）")
    itc = os.path.join(ROOT, "firmware", "User", "stm32f4xx_it.c")
    it_defs = set()
    if os.path.exists(itc):
        src = open(itc, encoding="utf-8", errors="replace").read()
        it_defs = set(re.findall(r"void\s+(\w+_Handler)\s*\(void\)", src))
    rtos_trio = {"SVC_Handler", "PendSV_Handler", "SysTick_Handler"}
    # 这个镜像是"带 FreeRTOS"还是"纯裸机"？由 map 里有没有 port.o 判定。
    # 纯裸机（make USE_RTOS=0）时，SVC/PendSV/SysTick 用 ST 模板的空实现是**正确**的。
    has_rtos = any(o.startswith("port") for o in owner.values())
    print(f"  （本次构建：{'带 FreeRTOS' if has_rtos else '纯裸机 USE_RTOS=0'}）")
    for nm_, why in REQUIRED_IRQS.items():
        addr = syms.get(nm_)
        if addr is None:
            print(f"  ⚠️  {nm_:32} 符号不存在（{why}）")
            required_state[nm_] = "missing"
            if nm_ != "HardFault_Handler":
                ok = False
            continue
        in_vec = any((w & ~1) == addr for w in words)
        is_default = (addr == default)
        obj = owner.get(nm_, "?")
        if is_default:
            tag = "Default_Handler（未实现）"
            mark = "✅" if nm_ == "HardFault_Handler" else "❌"
            if nm_ != "HardFault_Handler":
                ok = False
        elif obj.startswith("port"):
            tag = f"FreeRTOS 内核实现（{obj}）"
            mark = "✅"
        elif obj.endswith("_it.o"):
            if nm_ in rtos_trio and not has_rtos:
                tag = f"ST 模板空实现（{obj}）—— 纯裸机构建下正确"
                mark = "✅"
            else:
                tag = f"stm32f4xx_it.c 的 ST 模板实现（空/while(1)，{obj}）"
                mark = "⚠️" if nm_ not in rtos_trio else "❌"
                if nm_ in rtos_trio:
                    ok = False
        else:
            tag = f"真实实现（{obj}）"
            mark = "✅"
        print(f"  {mark} {nm_:32} 0x{addr:08X}  {tag}   ← {why}")
        required_state[nm_] = tag
        if not in_vec:
            print(f"      ⚠️  该函数没有出现在向量表里（名字对不上？）")

    missing_via_it = rtos_trio & it_defs
    if any(required_state.get(n, "").startswith("stm32f4xx_it") for n in rtos_trio) and missing_via_it:
        print("\n  ❗ FreeRTOS 的 SVC/PendSV/SysTick 被 stm32f4xx_it.c 里的 ST 模板占用了")
        print("     → 给 it.c 里这三个函数加 `#if !defined(USE_FREERTOS)` 保护，或改用 USE_RTOS=1 构建")

    print("\n# 结论：" + ("镜像向量表核验通过 ✅" if ok else "存在问题 ❌"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
