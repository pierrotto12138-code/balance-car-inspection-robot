#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""工具链探测 —— **不写死任何安装路径**，让脚本在任意机器上都能跑。

本模块解决两类查找：

  ┌─ HOST_CC：PC 端 C/C++ 编译器（跑原生单测用）
  │     顺序：环境变量 ``FW_HOST_CC`` → PATH 上的 ``gcc`` / ``cc`` / ``clang``
  │           → ``$QTDIR`` / ``$QT_ROOT`` 下的 ``Tools/mingw*/bin/gcc.exe``
  └─ ARM 工具链：``arm-none-eabi-gcc / objdump / nm / readelf / size``（构建与核验固件镜像）
        顺序：环境变量 ``FW_ARM_BIN``（目录）或 ``FW_ARM_PREFIX``（前缀）→ PATH 上的 ``arm-none-eabi-gcc``
              → 环境变量 ``STM32CUBECLT`` / ``STM32_CUBE_CLT`` 下的 ``GNU-tools-for-STM32/bin``

**装在哪里都可以**：只要把对应 ``bin`` 目录加进 ``PATH``，或设一个环境变量即可，例如

    set  FW_ARM_BIN=C:\\path\\to\\GNU-tools-for-STM32\\bin        # Windows cmd
    setx FW_HOST_CC=C:\\path\\to\\mingw64\\bin\\gcc.exe          # 长期生效
    export FW_ARM_BIN=/opt/gcc-arm-none-eabi/bin                  # Linux/macOS

供其他脚本使用：

    from toolchain import host_cc, arm_tool, require_arm, describe
    cc  = host_cc()                 # 找不到返回 None
    gcc = require_arm("gcc")        # 找不到会打印指引并 SystemExit(2)
"""

import glob
import os
import shutil
import subprocess
import sys

DEFAULT_ARM_PREFIX = "arm-none-eabi-"

# 交叉工具链的常见安装根（**相对探测，不针对某台机器**）：只要把 CubeCLT/CubeIDE
# 装到这些根目录下、或把 bin 加进 PATH / 设 FW_ARM_BIN，就能被找到。
_ARM_ROOT_GLOBS = [
    r"C:\ST\*",                       # ST 官方默认安装位置
    r"C:\Program Files\STMicroelectronics\*",
    r"C:\Program Files (x86)\STMicroelectronics\*",
    r"/opt/st/*",
    r"/usr/local/st/*",
]


def _works(cmd):
    """能在 PATH 上执行吗（执行 ``--version`` 验证，而不只是文件存在）。"""
    path = shutil.which(cmd) if not os.path.isabs(cmd) else (cmd if os.path.exists(cmd) else None)
    if not path:
        return None
    try:
        p = subprocess.run([path, "--version"], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if p.returncode == 0:
            return path
    except (OSError, subprocess.SubprocessError):
        return None
    return None


def host_cc():
    """PC 端 C 编译器路径；找不到返回 None（调用方负责给出指引）。"""
    env = os.environ.get("FW_HOST_CC")
    if env and _works(env):
        return _works(env)

    for name in ("gcc", "cc", "clang"):
        got = _works(name)
        if got:
            return got

    # Qt 自带的 MinGW（很多人机器上有），用环境变量定位，不做机器相关的硬编码
    for var in ("QTDIR", "QT_ROOT", "Qt6_DIR", "Qt5_DIR"):
        root = os.environ.get(var)
        if not root:
            continue
        for pat in ("Tools/mingw*/bin/gcc.exe", "Tools/mingw*/bin/gcc",
                    "../Tools/mingw*/bin/gcc.exe"):
            for cand in glob.glob(os.path.normpath(os.path.join(root, pat))):
                got = _works(cand)
                if got:
                    return got
    return None


def arm_prefix():
    """交叉工具链前缀（默认 ``arm-none-eabi-``，可用 ``FW_ARM_PREFIX`` 覆盖）。"""
    return os.environ.get("FW_ARM_PREFIX", DEFAULT_ARM_PREFIX)


def _arm_dirs():
    dirs = []
    for var in ("FW_ARM_BIN", "STM32CUBECLT", "STM32_CUBE_CLT", "STM32CUBEIDE",
                "STM32_CUBE_IDE"):
        val = os.environ.get(var)
        if not val:
            continue
        dirs.append(val)
        dirs.append(os.path.join(val, "bin"))
        # CubeCLT / CubeIDE 的典型内部结构
        dirs.append(os.path.join(val, "GNU-tools-for-STM32", "bin"))
        dirs.append(os.path.join(val, "STM32CubeIDE", "STM32CubeIDE", "plugins",
                                 "com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*", "tools", "bin"))
    # 常见安装根下的 glob（**不针对某台机器**）
    for root_glob in _ARM_ROOT_GLOBS:
        for root in glob.glob(root_glob):
            dirs.append(root)
            dirs.append(os.path.join(root, "bin"))
            dirs.append(os.path.join(root, "GNU-tools-for-STM32", "bin"))
            dirs.append(os.path.join(root, "STM32CubeCLT_*", "GNU-tools-for-STM32", "bin"))
    return [d for d in dirs if d]


def arm_tool(name):
    """查交叉工具链里的某个工具（如 ``"gcc"``/``"objdump"``/``"nm"``/``"size"``）。

    返回可执行文件路径；找不到返回 None。
    """
    prefix = arm_prefix()
    exe_suffix = ".exe" if os.name == "nt" else ""
    full = f"{prefix}{name}{exe_suffix}"

    for d in _arm_dirs():
        for pat in (os.path.join(d, full), os.path.join(d, "**", full)):
            for cand in glob.glob(pat, recursive=True):
                got = _works(cand)
                if got:
                    return got

    got = _works(full)
    if got:
        return got

    # 有些发行版不带前缀（少见），最后兜底试一下不带前缀的名字
    return _works(name) if name in ("gcc", "objdump", "nm", "readelf", "size") else None


def describe():
    """一行摘要，便于脚本打印环境信息。"""
    cc = host_cc()
    arm = arm_tool("gcc")
    return (f"HOST_CC = {cc or '未找到'}\n"
            f"ARM_GCC = {arm or '未找到'}")


def child_env(cc=None, extra_dirs=()):
    """返回一份用于**子进程**的环境变量。

    为什么需要：Windows 上的 MinGW/CLT 编译器并不是单文件 —— ``gcc.exe`` 启动的
    ``cc1.exe`` 依赖同目录下的 DLL（libgcc / libwinpthread 等）。
    如果只把编译器的**绝对路径**告诉脚本、而它的目录不在 ``PATH`` 上，
    子进程会"静默失败"（无输出、非零退出码），非常难查。
    这里统一把编译器自己的目录（以及交叉工具链目录）放到 ``PATH`` 最前面。
    """
    env = dict(os.environ)
    dirs = []
    cc = cc or host_cc()
    if cc:
        dirs.append(os.path.dirname(os.path.abspath(cc)))
    arm = arm_tool("gcc")
    if arm:
        dirs.append(os.path.dirname(os.path.abspath(arm)))
    dirs.extend(extra_dirs)
    if dirs:
        env["PATH"] = os.pathsep.join(dirs + [env.get("PATH", "")])
    return env


def _arm_hint():
    return (
        "未找到交叉工具链 arm-none-eabi-*。装在哪里都可以，任选一种方式让它可见：\n"
        "  1) 把它的 bin 目录加入 PATH；\n"
        "  2) 设环境变量 FW_ARM_BIN 指向 bin 目录，例如\n"
        "       Windows:  set FW_ARM_BIN=C:\\path\\to\\GNU-tools-for-STM32\\bin\n"
        "       Linux  :  export FW_ARM_BIN=/opt/gcc-arm-none-eabi/bin\n"
        "  3) 若用的是 STM32CubeCLT/CubeIDE，设 STM32CUBECLT 指向其安装目录（脚本会自动找 GNU-tools-for-STM32/bin）；\n"
        "  4) 也可用 FW_ARM_PREFIX 指定别的前缀（默认 arm-none-eabi-）。\n"
        "下载：https://github.com/ARM-software/arm-toolchain 或 STM32CubeCLT（ST 官网）。"
    )


def require_arm(name):
    """取交叉工具链工具；找不到则打印指引并 ``SystemExit(2)``。"""
    got = arm_tool(name)
    if not got:
        print(_arm_hint(), file=sys.stderr)
        raise SystemExit(2)
    return got


def require_host_cc():
    """取 PC 端编译器；找不到则打印指引并 ``SystemExit(2)``。"""
    got = host_cc()
    if not got:
        print("未找到 PC 端 C 编译器（gcc / cc / clang）。\n"
              "  任选一种：把它加入 PATH；或设 FW_HOST_CC 指向可执行文件，例如\n"
              "    Windows: set FW_HOST_CC=C:\\path\\to\\mingw64\\bin\\gcc.exe\n"
              "    Linux  : export FW_HOST_CC=/usr/bin/gcc", file=sys.stderr)
        raise SystemExit(2)
    return got


if __name__ == "__main__":
    print(describe())
