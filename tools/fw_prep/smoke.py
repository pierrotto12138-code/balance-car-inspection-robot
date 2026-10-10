#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""B 阶段开工冒烟测试：
1) 用 arm-none-eabi-gcc 构建一个最小 STM32F405 镜像（自写 startup + linker script）
2) 用 arm-none-eabi-gcc 逐个编译厂商基线的全部源文件，统计"可直接编译 / 需改造 / 失败"
   目的是量化"裸机基线移植到 GCC 工具链"的工作量，不修改任何原始文件。
"""
import os, re, subprocess, sys, glob

BASE = r"D:\260921Task\Base\高配版平衡小车附送资料（STM32F405RGT6）\MiniBalanceV5.0【高配版】平衡小车库函数版源码（卡尔曼滤波版）"
HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "out")
GCC = "arm-none-eabi-gcc"
SIZE = "arm-none-eabi-size"

CFLAGS = [
    "-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard",
    "-DSTM32F40_41xxx", "-DUSE_STDPERIPH_DRIVER",
    "-O0", "-g3", "-Wall", "-fno-common", "-ffunction-sections", "-fdata-sections",
    "-I" + os.path.join(BASE, "CORE"),
    "-I" + os.path.join(BASE, "FWLIB", "inc"),
    "-I" + os.path.join(BASE, "USER"),
    "-I" + os.path.join(BASE, "SYSTEM", "delay"),
    "-I" + os.path.join(BASE, "SYSTEM", "sys"),
    "-I" + os.path.join(BASE, "SYSTEM", "usart"),
    "-I" + os.path.join(BASE, "MiniBalance", "CONTROL"),
    "-I" + os.path.join(BASE, "MiniBalance", "filter"),
    "-I" + os.path.join(BASE, "MiniBalance", "MPU6050"),
    "-I" + os.path.join(BASE, "MiniBalance", "show"),
    "-I" + os.path.join(BASE, "MiniBalance", "DataScope_DP"),
    "-I" + os.path.join(BASE, "MiniBalance", "DMP"),
]
for d in ("ADC", "CAN", "ENCODER", "EXTI", "IIC", "KEY", "LED", "MOTOR",
          "NRF24L01", "OLED", "SPI", "STMFLASH", "TIMER", "USARTX"):
    CFLAGS.append("-I" + os.path.join(BASE, "MiniBalance_HARDWARE", d))


def run(cmd, cwd=None):
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def find_line(out, pat):
    for ln in out.splitlines():
        if re.search(pat, ln):
            return ln.strip()
    return ""


def smoke_build():
    os.makedirs(BUILD, exist_ok=True)
    main_c = os.path.join(HERE, "smoke_main.c")
    with open(main_c, "w", encoding="utf-8") as f:
        f.write("volatile unsigned int counter;\n"
                "int main(void){ for(;;){ counter++; } }\n")
    objs = []
    for src, lang in ((os.path.join(HERE, "startup_min.S"), "asm"),
                      (os.path.join(BASE, "USER", "system_stm32f4xx.c"), "c"),
                      (main_c, "c")):
        o = os.path.join(BUILD, os.path.basename(src) + ".o")
        rc, out = run([GCC] + CFLAGS + ["-c", src, "-o", o])
        print(f"  [{'OK ' if rc == 0 else 'FAIL'}] 编译 {os.path.relpath(src, BASE) if src.startswith(BASE) else os.path.basename(src)}")
        if rc != 0:
            print("        " + out.strip().splitlines()[0][:160])
            return False
        objs.append(o)
    elf = os.path.join(BUILD, "smoke.elf")
    rc, out = run([GCC, "-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16",
                   "-mfloat-abi=hard", "-nostartfiles", "-Wl,--gc-sections",
                   "-T" + os.path.join(HERE, "stm32f405rg_flash.ld"),
                   "-Wl,-Map=" + os.path.join(BUILD, "smoke.map"),
                   "-o", elf] + objs)
    if rc != 0:
        print("  [FAIL] 链接\n" + out[:800])
        return False
    rc, out = run([SIZE, elf])
    print("  链接 OK ->", os.path.relpath(elf))
    print("  " + out.strip().replace("\n", "\n  "))
    # 顺带验证"能出 hex/bin" —— 现场用 FlyMcu 走串口 ISP 烧的是 hex
    for ext, args in ((".hex", ["-O", "ihex"]), (".bin", ["-O", "binary"])):
        target = os.path.join(BUILD, "smoke" + ext)
        rc, out = run(["arm-none-eabi-objcopy"] + args + [elf, target])
        if rc == 0:
            print(f"  [OK ] arm-none-eabi-objcopy -> smoke{ext}  {os.path.getsize(target)} B")
        else:
            print(f"  [FAIL] objcopy {ext}: {out.strip()[:120]}")
    return True


def scan_vendor():
    srcs = []
    for root in (os.path.join(BASE, "FWLIB", "src"),
                 os.path.join(BASE, "SYSTEM"),
                 os.path.join(BASE, "USER"),
                 os.path.join(BASE, "MiniBalance"),
                 os.path.join(BASE, "MiniBalance_HARDWARE")):
        srcs += glob.glob(os.path.join(root, "**", "*.c"), recursive=True)
        srcs += glob.glob(os.path.join(root, "**", "*.C"), recursive=True)
    # Windows 文件系统大小写不敏感：*.c 与 *.C 会命中同一批文件，必须去重
    srcs = sorted({os.path.normcase(s): s for s in srcs}.values())
    srcs = [s for s in srcs
            if not re.search(r"\\(OBJ|DMP)\\", s) and not s.endswith("system_stm32f4xx.c")]
    ok, bad = [], []
    for s in srcs:
        o = os.path.join(BUILD, "scan_" + os.path.basename(s) + ".o")
        rc, out = run([GCC] + CFLAGS + ["-c", s, "-o", o])
        rel = os.path.relpath(s, BASE)
        if rc == 0:
            ok.append(rel)
        else:
            err = find_line(out, r"error:")
            bad.append((rel, err[:150] if err else out.strip().splitlines()[0][:150]))
    return ok, bad


def main():
    ver = run([GCC, "--version"])[1].splitlines()[0]
    print("工具链：", ver)
    print("\n[1] 最小镜像冒烟测试")
    ok = smoke_build()
    print("\n[2] 厂商基线源码在 arm-gcc 下的可编译性扫描")
    good, bad = scan_vendor()
    print(f"  可直接编译：{len(good)} 个   编译失败：{len(bad)} 个")
    print("\n  --- 失败清单（= 需要改造的移植点）---")
    for rel, err in bad:
        print(f"   ✗ {rel}\n       {err}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
