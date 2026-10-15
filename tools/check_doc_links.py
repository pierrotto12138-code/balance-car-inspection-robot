#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
文档链接断链检查（角色 C 的 P5 顺手工具，但整个仓库都能用）

为什么要有它：
    本项目出现过"文档整合时把图片挪了目录、链接没跟着改"的事 —— 而**断链在 Markdown 里
    是静默的**：编辑器不报、GitHub 上只是图不显示，直到评阅时才发现。
    眼睛扫一遍 `](...)` 是不可靠的（AGENTS §8 第 3 条就是这么写的），所以做成脚本。

它做什么：
    · 扫仓库里所有 `.md`（跳过 `Base/`、`.git/`、`archive/`）；
    · 提取 `[文字](目标)` 与 `![图](目标)`；只检查**相对路径**（http/https/mailto/#锚点 跳过）；
    · 逐条判断目标是否存在（同时容忍 URL 编码与 `%20` 空格）；
    · 有断链 ⇒ 打印 `文件:行号 → 目标` 并**退出码 1**；全通 ⇒ 退出码 0。

用法：
    python tools/check_doc_links.py            # 全仓库
    python tools/check_doc_links.py docs/host  # 只看某个目录
"""

import os
import re
import sys
import urllib.parse

# ★ 跳过的目录：
#   · Base/（学校原始资料，已 gitignore）、.git/、archive/（历史归档，保留原始引用不"修正"）
#   · 构建/打包产物目录
#   · **带时间戳的快照目录**（如 `firmware_工程_20261013_1804`）：快照是"当时的整目录拷贝"，
#     里面的相对链接指向快照外的文件，本来就不可能解析 —— 这一类断链是设计如此，不是缺陷。
SKIP_DIRS = {"Base", ".git", "archive", "node_modules", "build", "build-release", "dist", ".qt"}
SNAPSHOT_RE = re.compile(r"_\d{8}_\d{4}$")
LINK_RE = re.compile(r"!?\[(?P<text>[^\]]*)\]\((?P<target>[^)]+)\)")
SKIP_PREFIX = ("http://", "https://", "mailto:", "#", "data:")


def iter_markdown(roots):
    for root in roots:
        if os.path.isfile(root):
            yield root
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames
                           if d not in SKIP_DIRS and not SNAPSHOT_RE.search(d)]
            for name in filenames:
                if name.lower().endswith(".md"):
                    yield os.path.join(dirpath, name)


def check_file(path, repo_root):
    broken = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh, 1):
            for m in LINK_RE.finditer(line):
                target = m.group("target").strip()
                if not target or target.startswith(SKIP_PREFIX):
                    continue
                # 去掉锚点与标题部分：foo.md#L10 → foo.md
                raw = target.split("#", 1)[0].strip()
                if not raw:
                    continue
                # Markdown 链接里的空格可能写成 %20
                decoded = urllib.parse.unquote(raw)
                resolved = os.path.normpath(os.path.join(os.path.dirname(path), decoded))
                if not os.path.exists(resolved):
                    broken.append((lineno, target, os.path.relpath(resolved, repo_root)))
    return broken


def main():
    # 控制台默认不是 UTF-8（中文 Windows 是 GBK）⇒ 直接 print 中文会变乱码，
    # 而"报错信息本身乱码"等于没有报错信息（本项目在别处也踩过同类问题）。
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    roots = sys.argv[1:] or [repo_root]
    files = sorted(iter_markdown(roots))
    total_links = 0
    total_broken = 0
    for path in files:
        broken = check_file(path, repo_root)
        if broken:
            total_broken += len(broken)
            for lineno, target, resolved in broken:
                print("断链 {}:{} → {}  （解析为 {}）".format(
                    os.path.relpath(path, repo_root), lineno, target, resolved))
    # 统计一下总链接数（不是为了好看：这个数突然变小往往说明扫描范围被改错了）
    for path in files:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                total_links += len([m for m in LINK_RE.finditer(line)
                                    if not m.group("target").strip().startswith(SKIP_PREFIX)])
    print("扫描 {} 个 Markdown 文件 · 相对链接 {} 条 · 断链 {} 条".format(
        len(files), total_links, total_broken))
    return 1 if total_broken else 0


if __name__ == "__main__":
    sys.exit(main())
