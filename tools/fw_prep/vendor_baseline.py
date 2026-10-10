#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把厂商基线源码 vendor 进 `firmware/`（**GBK → UTF-8**，按新目录结构分层）。

为什么要脚本而不是手工复制：
    1. 基线文件是 **GBK** 编码，直接复制会让中文注释在接受 UTF-8 的编辑器里乱码，
       且后续 diff/检索都会失真 —— 必须显式转码并**报告转码失败的文件**；
    2. 映射关系（原目录 → 新目录）要可复现、可复核，不能靠手工拖拽；
    3. 基线在 `Base/`（**只读**），本脚本只读它、只写 `firmware/`。

用法：
    python tools/fw_prep/vendor_baseline.py            # 干跑，只打印计划
    python tools/fw_prep/vendor_baseline.py --apply    # 实际写入 firmware/
"""
import argparse
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(
    ROOT, "Base", "高配版平衡小车附送资料（STM32F405RGT6）",
    "MiniBalanceV5.0【高配版】平衡小车库函数版源码（卡尔曼滤波版）")
DST = os.path.join(ROOT, "firmware")

# (基线相对目录, firmware 目标目录, 说明)
MAP = [
    ("CORE", "Drivers/CMSIS", "CMSIS 内核头 + 启动汇编（Keil 语法，GCC 版另写）"),
    ("FWLIB/inc", "Drivers/FWLIB/inc", "ST 标准外设库头"),
    ("FWLIB/src", "Drivers/FWLIB/src", "ST 标准外设库源"),
    ("SYSTEM/sys", "System/sys", "位带宏、时钟（含 ARMCC 专有 __asm，需改）"),
    ("SYSTEM/delay", "System/delay", "延时"),
    ("SYSTEM/usart", "System/usart", "printf 重定向（含 ARMCC 半主机 pragma，需改）"),
    ("USER", "User", "main / 中断向量实现 / 配置头"),
    ("MiniBalance/CONTROL", "App/Control", "三环 PID + MPU6050 中断控制"),
    ("MiniBalance/filter", "App/Filter", "卡尔曼 / 互补滤波"),
    ("MiniBalance/MPU6050", "App/Sensor/MPU6050", "姿态传感器"),
    ("MiniBalance/show", "App/Ui", "OLED/APP 显示"),
    ("MiniBalance/DataScope_DP", "App/Comms/DataScope", "厂商波形协议（调试用）"),
    ("MiniBalance/DMP", "App/Sensor/DMP", "DMP 库（main 里仍调用 Dmp_Init）"),
    ("MiniBalance_HARDWARE/ADC", "Drivers/BSP/ADC", ""),
    ("MiniBalance_HARDWARE/CAN", "Drivers/BSP/CAN", ""),
    ("MiniBalance_HARDWARE/ENCODER", "Drivers/BSP/ENCODER", ""),
    ("MiniBalance_HARDWARE/EXTI", "Drivers/BSP/EXTI", ""),
    ("MiniBalance_HARDWARE/IIC", "Drivers/BSP/IIC", ""),
    ("MiniBalance_HARDWARE/KEY", "Drivers/BSP/KEY", ""),
    ("MiniBalance_HARDWARE/LED", "Drivers/BSP/LED", ""),
    ("MiniBalance_HARDWARE/MOTOR", "Drivers/BSP/MOTOR", ""),
    ("MiniBalance_HARDWARE/NRF24L01", "Drivers/BSP/NRF24L01", "已禁用，但 sys.h 会 include 其头文件，故一并 vendor"),
    ("MiniBalance_HARDWARE/OLED", "Drivers/BSP/OLED", ""),
    ("MiniBalance_HARDWARE/SPI", "Drivers/BSP/SPI", ""),
    ("MiniBalance_HARDWARE/STMFLASH", "Drivers/BSP/STMFLASH", ""),
    ("MiniBalance_HARDWARE/TIMER", "Drivers/BSP/TIMER", ""),
    ("MiniBalance_HARDWARE/USARTX", "Drivers/BSP/USARTX", "USART2 蓝牙 + APP 指令"),
]

# 不移植：NRF24L01（已禁用，本项目不用）、OBJ/（编译产物）、Keil 工程文件
SKIP_FILES = {"keilkilll.bat", "JLinkSettings.ini",
              "Minibalance.uvproj", "Minibalance.uvopt",
              "Minibalance_MinibalanceF4.dep", "Minibalance.map",
              "startup_stm32f40_41xxx.lst",
              "Minibalance.uvgui.passoni", "Minibalance_uvopt.bak",
              "Minibalance.uvgui_passoni.bak"}


def decode(path):
    """GBK 优先，失败退到 GB18030（超集），再失败返回 (None, 原因)。"""
    raw = open(path, "rb").read()
    for enc in ("gbk", "gb18030"):
        try:
            return raw.decode(enc), enc
        except UnicodeDecodeError:
            continue
    return None, "无法按 GBK/GB18030 解码"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true", help="真正写入（默认只干跑）")
    args = ap.parse_args()

    if not os.path.isdir(SRC):
        print(f"基线目录不存在：{SRC}")
        return 2

    n_copy = n_conv = n_skip = 0
    failed = []
    for rel, dstrel, note in MAP:
        sdir = os.path.join(SRC, rel.replace("/", os.sep))
        ddir = os.path.join(DST, dstrel.replace("/", os.sep))
        if not os.path.isdir(sdir):
            failed.append((rel, "源目录不存在"))
            continue
        files = sorted(f for f in os.listdir(sdir)
                       if os.path.isfile(os.path.join(sdir, f)))
        print(f"\n[{rel}] → [firmware/{dstrel}]  {len(files)} 个文件  {note}")
        for f in files:
            if f in SKIP_FILES or f.lower().endswith((".uvgui", ".bak", ".hex", ".o", ".d")):
                n_skip += 1
                print(f"    - 跳过 {f}")
                continue
            sp, dp = os.path.join(sdir, f), os.path.join(ddir, f)
            if f.lower().endswith((".c", ".h", ".cpp", ".txt", ".md")):
                text, enc = decode(sp)
                if text is None:
                    failed.append((os.path.join(rel, f), enc))
                    print(f"    ! 转码失败 {f}（{enc}）")
                    continue
                if args.apply:
                    os.makedirs(ddir, exist_ok=True)
                    with open(dp, "w", encoding="utf-8", newline="\n") as fh:
                        fh.write(text)
                n_conv += 1
                print(f"    + {f}   ({enc} → utf-8, {len(text)} 字符)")
            else:
                if args.apply:
                    os.makedirs(ddir, exist_ok=True)
                    shutil.copy2(sp, dp)
                n_copy += 1
                print(f"    + {f}   (二进制/原样复制)")

    print(f"\n合计：转码 {n_conv} 个、原样复制 {n_copy} 个、跳过 {n_skip} 个")
    if failed:
        print("需要人工处理：")
        for f, why in failed:
            print(f"  - {f}: {why}")
    if not args.apply:
        print("\n（干跑，未写入任何文件；加 --apply 执行）")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
