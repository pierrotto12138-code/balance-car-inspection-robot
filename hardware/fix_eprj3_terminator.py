"""补齐 .eprj3 源文件末行缺失的记录终止符 '|'。

背景：EDA 从 .eprj2 导出为 .eprj3 时，PCB 源文件的**最后一条记录漏写了结尾 '|'**
（原理图文件正常）。格式规范要求每行形如 {header}||{payload}|，缺少终止符会让
EDA 的解析器跳过该记录 —— 实测导致底层地平面（POUR IE1424 / GNDREF / layerId=2）
的覆铜填充整条不加载。

本脚本只做一件事：若最后一个非空行不以 '|' 结尾，就补上。
其余字节原样保留。

用法：
    python fix_eprj3_terminator.py --project "<工程文件夹>"            # 预演
    python fix_eprj3_terminator.py --project "<工程文件夹>" --apply    # 执行（先备份）
"""
import argparse
import shutil
import sys
import time
from pathlib import Path

SUFFIXES = (".esch2", ".epcb2", ".epan2")


def contained(base: Path, target: Path) -> Path:
    base_r = base.resolve()
    tr = target.resolve()
    tr.relative_to(base_r)
    return tr


def inspect(path: Path):
    """Return (needs_fix, info) without modifying anything."""
    raw = path.read_bytes()
    # 保留原有换行风格
    crlf = b"\r\n" in raw
    nl = b"\r\n" if crlf else b"\n"
    ends_with_nl = raw.endswith(nl)
    body = raw[:-len(nl)] if ends_with_nl else raw
    lines = body.split(nl)
    # 找最后一个非空行
    last_idx = None
    for i in range(len(lines) - 1, -1, -1):
        if lines[i].strip():
            last_idx = i
            break
    if last_idx is None:
        return False, {"reason": "空文件"}
    last = lines[last_idx]
    ok = last.rstrip().endswith(b"|")
    info = {
        "lines": len(lines),
        "last_idx": last_idx,
        "last_len": len(last),
        "last_head": last[:110].decode("utf-8", "replace"),
        "ends_with_nl": ends_with_nl,
        "crlf": crlf,
        "ok": ok,
    }
    return (not ok), info


def fix(path: Path, project_root: Path, backup_root: Path):
    src = contained(project_root, path)
    raw = src.read_bytes()
    crlf = b"\r\n" in raw
    nl = b"\r\n" if crlf else b"\n"
    ends_with_nl = raw.endswith(nl)
    body = raw[:-len(nl)] if ends_with_nl else raw
    lines = body.split(nl)
    for i in range(len(lines) - 1, -1, -1):
        if lines[i].strip():
            lines[i] = lines[i] + b"|"
            break
    new_body = nl.join(lines)
    if ends_with_nl:
        new_body += nl
    backup_root.mkdir(parents=True, exist_ok=True)
    dst = contained(backup_root, backup_root / src.name)
    shutil.copy2(src, dst)
    src.write_bytes(new_body)
    return dst, len(new_body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--project", required=True)
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    root = Path(args.project).resolve()
    if not root.is_dir():
        print("工程目录不存在: %s" % root)
        return 2

    targets = sorted(p for p in root.rglob("*")
                     if p.is_file() and p.name.endswith(SUFFIXES))
    if not targets:
        print("没找到源文件")
        return 2

    todo = []
    for t in targets:
        contained(root, t)
        need, info = inspect(t)
        print("\n%-26s %s" % (t.name, "需要补 '|'" if need else "结尾正常 ✓"))
        for k, v in info.items():
            print("     %-14s %s" % (k, v))
        if need:
            todo.append(t)

    if not todo:
        print("\n无需修复。")
        return 0
    if not args.apply:
        print("\n（预演模式。加 --apply 执行。）")
        return 0

    stamp = time.strftime("%Y%m%d-%H%M%S")
    bdir = root.parent / ("_eprj3_terminator_backup_%s_%s" % (root.name, stamp))
    print("\n备份到: %s" % bdir)
    for t in todo:
        dst, size = fix(t, root, bdir)
        print("  已修复 %-24s 新大小=%d  → 备份 %s" % (t.name, size, dst.name))
    print("\n完成。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
