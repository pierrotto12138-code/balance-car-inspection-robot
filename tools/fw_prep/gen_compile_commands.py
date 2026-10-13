# -*- coding: utf-8 -*-
"""
生成 compile_commands.json（给 VS Code / clangd / CLion 做代码跳转与补全用）

做法：跑 `make -n` 把 Makefile 真正会执行的编译命令**抓下来**（不真编译），
      把其中的 .c 源文件逐个写成编译数据库条目。

为什么不用 bear / compiledb：本项目已经有可用的 make，抓 `make -n` 就够了，
不引入额外依赖（本机也没有 bear）。

用法：
    cd firmware
    make compile_commands        # 推荐（Makefile 里的封装目标）
    python ../tools/fw_prep/gen_compile_commands.py    # 等价

产物：firmware/compile_commands.json（已加入 .gitignore，属本机生成物）
"""
import io
import json
import os
import re
import subprocess
import sys

FW = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FW = os.path.join(FW, "firmware")
OUT = os.path.join(FW, "compile_commands.json")


def main():
    make = os.environ.get("MAKE", "make")
    # ★ 必须显式指定 `all` 目标 + `-B`：
    #   · 不指定目标 → make 会挑默认目标，而 Makefile 里默认目标不一定是 all；
    #   · 不加 -B    → 若 build/ 已存在，make 认为无需重编，`-n` 什么都不打印。
    try:
        raw = subprocess.run([make, "-n", "-B", "all"], cwd=FW, capture_output=True, text=True,
                             encoding="utf-8", errors="replace")
    except FileNotFoundError:
        print("找不到 make：请确认 STM32CubeCLT 的 Make 在 PATH 里")
        return 1
    if raw.returncode != 0:
        print("make -n -B all 失败：\n" + (raw.stderr or "")[:2000])
        return 1

    entries = []
    seen = set()
    for line in raw.stdout.split("\n"):
        line = line.strip()
        # 只取"编译某个 .c"的命令行（链接/objcopy/汇编等跳过）
        #
        # ★ 注意：**不能**用 `-c\s+\S+\.c` 去匹配。
        #   Makefile 里是 `$(CC) -c $(CFLAGS) $< -o $@` —— `-c` 紧跟在编译器后面，
        #   之后是一长串 `-m*/-I/-D/...`，`.c` 源文件在**行尾附近**。
        #   所以按"空白分词 → 找不以 '-' 开头、且真实存在的 .c 文件"来定位源文件。
        if " -c " not in line:
            continue
        src = None
        for tok in line.split():
            if tok.startswith("-") or not tok.endswith(".c"):
                continue
            if os.path.exists(os.path.join(FW, tok)):
                src = tok
                break
        if src is None or src in seen:
            continue
        seen.add(src)
        entries.append({
            # clangd/编辑器要求 directory + file 都是**绝对路径**，且用 '/' 分隔
            "directory": FW.replace("\\", "/"),
            "file": os.path.abspath(os.path.join(FW, src)).replace("\\", "/"),
            "command": line,
        })

    if not entries:
        print("没抓到任何编译命令 —— Makefile 结构变了？请检查 make -n 的输出")
        return 1

    with io.open(OUT, "w", encoding="utf-8", newline="\n") as f:
        json.dump(entries, f, ensure_ascii=False, indent=1)
    print("已生成 %s（%d 个编译单元）" % (os.path.relpath(OUT), len(entries)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
