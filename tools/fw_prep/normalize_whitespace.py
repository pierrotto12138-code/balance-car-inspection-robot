# -*- coding: utf-8 -*-
"""
P3.2 空白规范化（**只动空白，不动任何非空白字符**）

做什么：
  1) 行尾统一 LF（仓库 .gitattributes 规定 *.c/*.h/Makefile 一律 eol=lf）
  2) 去掉行尾空白
  3) 文件结尾恰好一个换行
  4) 混合缩进（Tab 后面又跟空格）→ 统一成纯 Tab
     ★ 只在"该行缩进里同时有 Tab 和空格"时改写，纯空格缩进的行**原样保留**
       （避免把别人刻意用空格对齐的地方搅乱，也让 diff 尽量小）

安全阀：改写完成后逐个文件比对"去掉所有空白字符后的内容"，
        必须与原文件完全一致 —— 任何非空白变化都会让脚本报错退出。
"""
import io
import os
import re
import sys

# ★ 路径不写死：从脚本自身位置推出 firmware/（本脚本位于 <仓库根>/tools/fw_prep/）
#   —— 原先这里写死了某一台机器的绝对路径，违反"仓库通用化"约定（2026-10-13 修正）。
#   也可以显式指定目标目录：python normalize_whitespace.py <firmware 目录>
ROOT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "firmware")
if len(sys.argv) > 1 and os.path.isdir(sys.argv[1]):
    ROOT = os.path.abspath(sys.argv[1])
EXTS = {".c", ".h"}

# ★ 作用域白名单：**只碰我们自己的 / 我们手改过的文件**。
#   厂商与第三方一律原样保留（编码规范 §一：厂商代码只做必要移植改动并标 ★）：
#     Drivers/FWLIB      标准外设库
#     Drivers/CMSIS      ARM/ST 内核与启动
#     Middlewares        FreeRTOS 官方内核
#     App/Sensor/DMP     InvenSense 官方 DMP 驱动
#     App/Comms/DataScope 厂商 DataScope 波形协议
#     User/stm32f4xx*     ST 官方设备头/系统文件
ALLOW = (
    r"App\Board",
    r"App\Comms\comm_link",
    r"App\Comms\crc16",
    r"App\Comms\protocol",
    r"App\Comms\ringbuf",
    r"App\Config",
    r"App\Control",
    r"App\Filter",
    r"App\Sensor\dht11",
    r"App\Sensor\gas",
    r"App\Sensor\sensor_adc",
    r"App\Sensor\sensor_report",
    r"App\Sensor\MPU6050",
    r"App\Task",
    r"App\Ui",
    r"Drivers\BSP\ADC",
    r"Drivers\BSP\KEY",
    r"Drivers\BSP\LED",
    r"Drivers\BSP\MOTOR",
    r"Drivers\BSP\OLED\oled",
    r"Drivers\BSP\STMFLASH",
    r"Drivers\BSP\USARTX",
    r"System\delay",
    r"System\usart",
    r"System\sys",
    r"User\main",
    r"User\rtos_hooks",
    r"Makefile",
)


def in_scope(rel):
    return any(rel.startswith(a) for a in ALLOW)


def normalize(text):
    # 行尾统一 LF
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    out = []
    for line in text.split("\n"):
        # 先去掉行尾空白（Tab/空格）
        line = line.rstrip(" \t")
        # 混合缩进 → 纯 Tab：
        #   仅当"行首缩进里同时出现 Tab 和空格"时才改写（纯 Tab / 纯空格的行原样保留）。
        #   折算：Tab 数 + ceil(空格数 / 4)。为什么向上取整：
        #     `\t  x`（Tab + 2 空格）在原文件里的意图是"比上一层多缩进一级"，
        #     按 4 取整会剩 1~3 个空格 → 等于没改；向上取整才能落成干净的下一级 Tab。
        m = re.match(r"^([ \t]+)(.*)$", line)
        if m:
            indent, rest = m.group(1), m.group(2)
            if "\t" in indent and " " in indent:
                tabs = indent.count("\t")
                spaces = indent.count(" ")
                indent = "\t" * (tabs + (spaces + 3) // 4)
                line = indent + rest
        out.append(line)
    text = "\n".join(out)
    # 结尾恰好一个换行
    text = text.rstrip("\n") + "\n"
    return text


def strip_all_ws(text):
    return re.sub(r"\s+", "", text)


def main():
    changed = []
    checked = 0
    for dirpath, dirnames, filenames in os.walk(ROOT):
        if "build" in dirpath or ".git" in dirpath:
            continue
        for name in filenames:
            ext = os.path.splitext(name)[1]
            if ext not in EXTS and name != "Makefile":
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, ROOT)
            if not in_scope(rel):
                continue
            with io.open(path, "r", encoding="utf-8", errors="strict") as f:
                src = f.read()
            dst = normalize(src)
            checked += 1
            if dst != src:
                if strip_all_ws(dst) != strip_all_ws(src):
                    print("!! 非空白内容被改动，拒绝写入: %s" % path)
                    return 1
                with io.open(path, "w", encoding="utf-8", newline="\n") as f:
                    f.write(dst)
                changed.append(rel)
    print("检查 %d 个文件，规范化 %d 个" % (checked, len(changed)))
    for c in sorted(changed):
        print("   " + c)
    return 0


if __name__ == "__main__":
    sys.exit(main())
